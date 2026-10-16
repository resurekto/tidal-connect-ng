#include "media/audio/AACDecoder.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include "util/Log.hpp"

namespace tcng::media {

namespace {
constexpr int kAvioBufferSize = 0x8000;  // 32 KiB
}

AACDecoder::AACDecoder(IInputStream& input, IAudioOutput& output) : input_(input), output_(output) {
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    resetFormatContext();
}

AACDecoder::~AACDecoder() {
    closeCodec();
    if (format_) {
        if (opened_)
            avformat_close_input(&format_);
        else
            avformat_free_context(format_);
    }
    if (avio_) {
        av_freep(&avio_->buffer);
        avio_context_free(&avio_);
    }
    av_packet_free(&packet_);
    av_frame_free(&frame_);
}

// AVIO read callback. Returns AVERROR_EOF instead of 0 when nothing
// is buffered (0 is not a valid result for current FFmpeg).
int AACDecoder::readPacket(void* opaque, std::uint8_t* buf, int size) {
    auto* self = static_cast<AACDecoder*>(opaque);
    const std::size_t n = self->input_.read(reinterpret_cast<char*>(buf), static_cast<std::size_t>(size));
    return n ? static_cast<int>(n) : AVERROR_EOF;
}

// AVIO seek callback. Honours whence.
std::int64_t AACDecoder::seekStream(void* opaque, std::int64_t offset, int whence) {
    auto* self = static_cast<AACDecoder*>(opaque);
    const auto size = static_cast<std::int64_t>(self->input_.size());
    if (whence & AVSEEK_SIZE)
        return size ? size : -1;
    std::int64_t target = offset;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_CUR:
        target = static_cast<std::int64_t>(self->input_.position()) + offset;
        break;
    case SEEK_END:
        if (!size)
            return -1;
        target = size + offset;
        break;
    default:
        break;
    }
    if (target < 0)
        return -1;
    self->input_.seek(static_cast<std::uint64_t>(target));
    return target;
}

void AACDecoder::closeCodec() {
    if (swr_)
        swr_free(&swr_);
    if (codec_)
        avcodec_free_context(&codec_);
}

void AACDecoder::resetFormatContext() {
    closeCodec();
    if (format_) {
        if (opened_)
            avformat_close_input(&format_);
        else
            avformat_free_context(format_);
        format_ = nullptr;
    }
    opened_ = false;
    if (avio_) {
        av_freep(&avio_->buffer);
        avio_context_free(&avio_);
    }
    auto* buffer = static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
    avio_ = avio_alloc_context(buffer, kAvioBufferSize, 0, this, &AACDecoder::readPacket, nullptr,
                               &AACDecoder::seekStream);
    input_.seek(0);
    format_ = avformat_alloc_context();
    format_->pb = avio_;
    format_->flags |= AVFMT_FLAG_CUSTOM_IO;
}

DecoderStatus AACDecoder::parseMetadata() {
    if (metadata_.valid())
        return DecoderStatus::Success;
    if (!opened_) {
        const int rc = avformat_open_input(&format_, "", nullptr, nullptr);
        if (rc < 0) {
            // avformat_open_input frees the context on failure.
            format_ = nullptr;
            const bool needMore = !input_.complete();
            resetFormatContext();
            if (needMore)
                return DecoderStatus::Underrun;
            log::get()->error("AACDecoder: avformat_open_input failed ({})", rc);
            return DecoderStatus::CorruptedMetadata;
        }
        opened_ = true;
    }
    if (avformat_find_stream_info(format_, nullptr) < 0) {
        if (!input_.complete()) {
            resetFormatContext();
            return DecoderStatus::Underrun;
        }
        return DecoderStatus::CorruptedMetadata;
    }
    const AVCodec* decoder = nullptr;
    streamIndex_ = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (streamIndex_ < 0 || !decoder)
        return DecoderStatus::CorruptedMetadata;
    codec_ = avcodec_alloc_context3(decoder);
    if (!codec_ || avcodec_parameters_to_context(codec_, format_->streams[streamIndex_]->codecpar) < 0 ||
        avcodec_open2(codec_, decoder, nullptr) < 0) {
        closeCodec();
        return DecoderStatus::CorruptedStream;
    }
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout inLayout{};
    if (codec_->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&inLayout, &codec_->ch_layout);
    else
        av_channel_layout_default(&inLayout, 2);
    if (inLayout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&inLayout, inLayout.nb_channels);
    const int rc = swr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_S16, codec_->sample_rate, &inLayout,
                                       codec_->sample_fmt, codec_->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&inLayout);
    if (rc < 0 || swr_init(swr_) < 0) {
        closeCodec();
        return DecoderStatus::CorruptedStream;
    }
    metadata_.clear();
    metadata_.sampleRatePlayback = static_cast<unsigned>(codec_->sample_rate);
    metadata_.channels = 2;
    metadata_.bitsPerSample = 16;
    metadata_.packaging = Packaging::P16BitsIn16BitsContainer;
    metadata_.interleaved = true;
    if (format_->duration > 0)
        totalFrames_ = static_cast<std::uint64_t>(static_cast<double>(format_->duration) * codec_->sample_rate / AV_TIME_BASE);
    log::get()->debug("AACDecoder: rate={} channels={} totalFrames={}", codec_->sample_rate,
                    codec_->ch_layout.nb_channels, totalFrames_);
    return DecoderStatus::Success;
}

std::uint64_t AACDecoder::drainFrames() {
    std::uint64_t produced = 0;
    std::vector<std::uint8_t> out;
    while (avcodec_receive_frame(codec_, frame_) == 0) {
        const int maxOut = swr_get_out_samples(swr_, frame_->nb_samples);
        out.resize(static_cast<std::size_t>(std::max(maxOut, 0)) * 2 * sizeof(std::int16_t));
        std::uint8_t* outPlanes[1] = {out.data()};
        const int n = swr_convert(swr_, outPlanes, maxOut, const_cast<const std::uint8_t**>(frame_->extended_data),
                                  frame_->nb_samples);
        std::size_t first = 0;
        std::size_t count = n > 0 ? static_cast<std::size_t>(n) : 0;
        if (skip_ != 0 && count != 0) {
            first = static_cast<std::size_t>(std::min<std::uint64_t>(skip_, count));
            skip_ -= first;
            count -= first;
        }
        if (count != 0) {
            output_.appendInterleaved(out.data() + first * 2 * sizeof(std::int16_t), count, metadata_);
            produced += count;
        }
        av_frame_unref(frame_);
    }
    return produced;
}

DecoderStatus AACDecoder::decode(unsigned frames) {
    if (!codec_)
        return DecoderStatus::CorruptedMetadata;
    if (input_.availableBytes() / (2ull * metadata_.channels) < frames && !input_.complete())
        return DecoderStatus::Underrun;
    if (format_->pb)
        format_->pb->eof_reached = 0;  // data may have arrived since the last short read
    std::uint64_t produced = 0;
    while (produced < frames) {
        const int rc = av_read_frame(format_, packet_);
        if (rc < 0) {
            // TCNG: only a real end of input ends the track; a short read is an underrun.
            if (rc == AVERROR_EOF && input_.complete()) {
                avcodec_send_packet(codec_, nullptr);  // flush the decoder
                produced += drainFrames();
                ended_ = true;
                return produced ? DecoderStatus::Success : DecoderStatus::Complete;
            }
            return produced ? DecoderStatus::Success : DecoderStatus::Underrun;
        }
        if (packet_->stream_index == streamIndex_ && avcodec_send_packet(codec_, packet_) == 0)
            produced += drainFrames();
        av_packet_unref(packet_);
    }
    return DecoderStatus::Success;
}

// seek: frame index -> stream time base.
DecoderStatus AACDecoder::seek(std::uint64_t frame) {
    if (!codec_ || streamIndex_ < 0)
        return DecoderStatus::Underrun;
    const AVStream* stream = format_->streams[streamIndex_];
    const std::int64_t ts = av_rescale_q(static_cast<std::int64_t>(frame), AVRational{1, codec_->sample_rate}, stream->time_base);
    if (format_->pb)
        format_->pb->eof_reached = 0;
    if (av_seek_frame(format_, streamIndex_, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return DecoderStatus::CorruptedStream;
    avcodec_flush_buffers(codec_);
    swr_init(swr_);
    ended_ = false;
    return DecoderStatus::Success;
}

unsigned AACDecoder::outputFrameSize() const {
    return codec_ ? static_cast<unsigned>(codec_->frame_size) : 0;
}

}  // namespace tcng::media
