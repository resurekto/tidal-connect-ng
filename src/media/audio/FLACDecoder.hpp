#pragma once
// FLACDecoder: a FLAC++ stream decoder reading from the IInputStream and writing
// interleaved stereo (mono duplicated) to the IAudioOutput: S16LE for <= 16-bit sources,
// packed S24_3LE for 17..24-bit sources.

#include <cstdint>
#include <vector>

#include <FLAC++/decoder.h>

#include "media/audio/Interfaces.hpp"

namespace tcng::media {

class FLACDecoder : public IAudioDecoder, private FLAC::Decoder::Stream {
public:
    FLACDecoder(IInputStream& input, IAudioOutput& output);
    ~FLACDecoder() override;

    const AudioMetadata& metadata() const override { return metadata_; }
    DecoderStatus parseMetadata() override;
    DecoderStatus decode(unsigned frames) override;
    DecoderStatus seek(std::uint64_t frame) override;
    unsigned outputFrameSize() const override { return maxBlocksize_; }
    bool ended() const override;
    void skipFrames(std::uint64_t frames) override { skip_ = frames; }
    std::uint64_t totalFrames() const override;

private:
    // FLAC::Decoder::Stream callbacks
    ::FLAC__StreamDecoderReadStatus read_callback(FLAC__byte buffer[], size_t* bytes) override;
    ::FLAC__StreamDecoderSeekStatus seek_callback(FLAC__uint64 absolute_byte_offset) override;
    ::FLAC__StreamDecoderTellStatus tell_callback(FLAC__uint64* absolute_byte_offset) override;
    ::FLAC__StreamDecoderLengthStatus length_callback(FLAC__uint64* stream_length) override;
    bool eof_callback() override;
    ::FLAC__StreamDecoderWriteStatus write_callback(const ::FLAC__Frame* frame, const FLAC__int32* const buffer[]) override;
    void metadata_callback(const ::FLAC__StreamMetadata* metadata) override;
    void error_callback(::FLAC__StreamDecoderErrorStatus status) override;

    // Enough input for one more frame: whole file present, or more than max_framesize.
    bool readableStream() const;
    void writeFrame(const FLAC__int32* const buffer[], unsigned samples, unsigned channels, unsigned bits);

    IInputStream& input_;
    IAudioOutput& output_;
    AudioMetadata metadata_;
    bool gotStreamInfo_ = false;
    bool error_ = false;
    bool starved_ = false;  // read_callback aborted for lack of data
    unsigned maxFramesize_ = 0;
    unsigned maxBlocksize_ = 0;
    unsigned streamChannels_ = 0;
    unsigned sourceBits_ = 0;
    std::uint64_t produced_ = 0;  // frames written during the current decode() call
    std::uint64_t skip_ = 0;      // frames still to drop (skipFrames)
    std::vector<std::uint8_t> scratch_;
};

}  // namespace tcng::media
