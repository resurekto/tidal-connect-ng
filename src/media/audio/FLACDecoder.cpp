#include "media/audio/FLACDecoder.hpp"

#include <algorithm>

#include "util/Log.hpp"

namespace tcng::media {

FLACDecoder::FLACDecoder(IInputStream& input, IAudioOutput& output) : input_(input), output_(output) {
    set_metadata_respond_all();
}

FLACDecoder::~FLACDecoder() {
    finish();
}

// --- callbacks ------------------------------------------------

::FLAC__StreamDecoderReadStatus FLACDecoder::read_callback(FLAC__byte buffer[], size_t* bytes) {
    if (*bytes && input_.endOfStream()) {
        *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    if (input_.availableBytes()) {
        *bytes = input_.read(reinterpret_cast<char*>(buffer), *bytes);
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }
    starved_ = true;
    *bytes = 0;
    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
}

::FLAC__StreamDecoderSeekStatus FLACDecoder::seek_callback(FLAC__uint64 offset) {
    input_.seek(offset);
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

::FLAC__StreamDecoderTellStatus FLACDecoder::tell_callback(FLAC__uint64* offset) {
    *offset = input_.position();
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

::FLAC__StreamDecoderLengthStatus FLACDecoder::length_callback(FLAC__uint64* length) {
    *length = input_.size();
    return *length ? FLAC__STREAM_DECODER_LENGTH_STATUS_OK : FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED;
}

bool FLACDecoder::eof_callback() {
    return input_.endOfStream();
}

void FLACDecoder::metadata_callback(const ::FLAC__StreamMetadata* md) {
    if (gotStreamInfo_)
        return;
    if (md->type != FLAC__METADATA_TYPE_STREAMINFO) {
        error_ = true;
        return;
    }
    const auto& si = md->data.stream_info;
    maxFramesize_ = si.max_framesize;
    maxBlocksize_ = si.max_blocksize;
    streamChannels_ = si.channels;
    sourceBits_ = si.bits_per_sample;
    metadata_.clear();
    metadata_.channels = 2;  // forced stereo; mono is duplicated
    metadata_.sampleRatePlayback = si.sample_rate;
    metadata_.interleaved = true;
    // <= 16 bits go out as S16 and 17..24 bits as packed S24 (left-aligned); deeper sources
    // are reported as corrupt.
    if (sourceBits_ <= 16) {
        metadata_.bitsPerSample = 16;
        metadata_.packaging = Packaging::P16BitsIn16BitsContainer;
    } else if (sourceBits_ <= 24) {
        metadata_.bitsPerSample = 24;
        metadata_.packaging = Packaging::P24BitsIn24BitsContainer;
    } else {
        log::get()->error("FLACDecoder: unsupported bits per sample {}", sourceBits_);
        error_ = true;
    }
    log::get()->debug("FLACDecoder: STREAMINFO rate={} bits={} channels={} total={}", si.sample_rate, sourceBits_,
                    streamChannels_, static_cast<std::uint64_t>(si.total_samples));
    gotStreamInfo_ = true;
}

void FLACDecoder::error_callback(::FLAC__StreamDecoderErrorStatus status) {
    log::get()->warn("FLACDecoder error: {}", FLAC__StreamDecoderErrorStatusString[status]);
    if (status == FLAC__STREAM_DECODER_ERROR_STATUS_UNPARSEABLE_STREAM)
        error_ = true;
}

::FLAC__StreamDecoderWriteStatus FLACDecoder::write_callback(const ::FLAC__Frame* frame,
                                                             const FLAC__int32* const buffer[]) {
    writeFrame(buffer, frame->header.blocksize, frame->header.channels, sourceBits_);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

// writeFrame: L = ch0, R = ch1 (or ch0 for mono), interleaved little endian.
void FLACDecoder::writeFrame(const FLAC__int32* const buffer[], unsigned samples, unsigned channels, unsigned bits) {
    if (!metadata_.valid() || samples == 0)
        return;
    unsigned first = 0;
    if (skip_ != 0) {
        const auto dropped = static_cast<unsigned>(std::min<std::uint64_t>(skip_, samples));
        skip_ -= dropped;
        if (dropped == samples)
            return;
        first = dropped;
        samples -= dropped;
    }
    const FLAC__int32* left = buffer[0] + first;
    const FLAC__int32* right = (channels >= 2 ? buffer[1] : buffer[0]) + first;
    const unsigned bytes = bytesPerSample(metadata_.packaging);
    const unsigned shift = metadata_.bitsPerSample - bits;
    scratch_.resize(static_cast<std::size_t>(samples) * 2 * bytes);
    std::uint8_t* out = scratch_.data();
    for (unsigned i = 0; i < samples; ++i) {
        for (const FLAC__int32 s : {left[i], right[i]}) {
            const auto v = static_cast<std::uint32_t>(s) << shift;
            out[0] = static_cast<std::uint8_t>(v);
            out[1] = static_cast<std::uint8_t>(v >> 8);
            if (bytes == 3)
                out[2] = static_cast<std::uint8_t>(v >> 16);
            out += bytes;
        }
    }
    output_.appendInterleaved(scratch_.data(), samples, metadata_);
    produced_ += samples;
}

// --- IAudioDecoder ------------------------------------------------------------------

bool FLACDecoder::readableStream() const {
    return input_.complete() || (maxFramesize_ != 0 && input_.availableBytes() > maxFramesize_);
}

// parseMetadata: 1 parsed, 2 corrupt, 4 needs more data.
DecoderStatus FLACDecoder::parseMetadata() {
    if (metadata_.valid() && gotStreamInfo_)
        return DecoderStatus::Success;
    if (get_state() == FLAC__STREAM_DECODER_UNINITIALIZED) {
        if (init() != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
            log::get()->error("FLACDecoder: init failed");
            return DecoderStatus::CorruptedMetadata;
        }
    }
    starved_ = false;
    const bool ok = process_until_end_of_metadata();
    if (error_)
        return DecoderStatus::CorruptedMetadata;
    if (ok && gotStreamInfo_ && get_state() != FLAC__STREAM_DECODER_ABORTED)
        return metadata_.valid() ? DecoderStatus::Success : DecoderStatus::CorruptedMetadata;
    if (starved_ || get_state() == FLAC__STREAM_DECODER_ABORTED) {
        // Restart from the beginning once more data is in (AudioWorker seeks to 0).
        gotStreamInfo_ = false;
        metadata_.clear();
        reset();
        return DecoderStatus::Underrun;
    }
    return DecoderStatus::CorruptedMetadata;
}

DecoderStatus FLACDecoder::decode(unsigned frames) {
    if (!metadata_.valid())
        return DecoderStatus::CorruptedMetadata;
    if (get_state() == FLAC__STREAM_DECODER_ABORTED)
        flush();  // resume after a starved read
    produced_ = 0;
    while (produced_ < frames) {
        const auto state = get_state();
        if (state == FLAC__STREAM_DECODER_END_OF_STREAM || state == FLAC__STREAM_DECODER_ABORTED || error_ ||
            !readableStream())
            break;
        if (!process_single())
            break;
    }
    FLAC__uint64 pos = 0;
    if (get_decode_position(&pos))
        input_.setDecodedPosition(pos);
    if (error_)
        return DecoderStatus::CorruptedStream;
    if (produced_ == 0)
        return ended() ? DecoderStatus::Complete : DecoderStatus::Underrun;
    return DecoderStatus::Success;
}

// seek: absolute frame index.
DecoderStatus FLACDecoder::seek(std::uint64_t frame) {
    flush();
    starved_ = false;
    const bool ok = seek_absolute(frame);
    if (ok && get_state() != FLAC__STREAM_DECODER_SEEK_ERROR)
        return DecoderStatus::Success;
    flush();
    if (starved_)
        return DecoderStatus::Underrun;
    return error_ ? DecoderStatus::CorruptedStream : DecoderStatus::Underrun;
}

bool FLACDecoder::ended() const {
    return get_state() == FLAC__STREAM_DECODER_END_OF_STREAM;
}

std::uint64_t FLACDecoder::totalFrames() const {
    return get_total_samples();
}

}  // namespace tcng::media
