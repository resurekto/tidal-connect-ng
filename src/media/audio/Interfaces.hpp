#pragma once
// Decoder-side interfaces: IInputStream (StreamManager),
// IAudioOutput (AudioOutputBuffer) and IAudioDecoder (FLACDecoder, AACDecoder).

#include <cstddef>
#include <cstdint>

#include "media/audio/Formats.hpp"

namespace tcng::media {

class IInputStream {
public:
    virtual ~IInputStream() = default;
    virtual void setDecodedPosition(std::uint64_t position) = 0;
    virtual void setStreamFormat(StreamFormat format) = 0;
    virtual StreamFormat format() const = 0;
    virtual bool endOfStream() const = 0;
    virtual bool complete() const = 0;
    virtual std::uint64_t availableBytes() const = 0;
    virtual std::uint64_t position() const = 0;
    virtual std::uint64_t size() const = 0;                       // 0 = unknown
    virtual std::size_t read(char* dst, std::size_t n) = 0;
    virtual std::int64_t seek(std::uint64_t position) = 0;        // -1 if not buffered
};

class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;
    // Interleaved samples, frames * channels of bytesPerSample(md.packaging). Returns frames stored.
    virtual std::size_t appendInterleaved(const void* data, std::size_t frames, const AudioMetadata& md) = 0;
    virtual bool isLastOutput() const = 0;
    virtual std::size_t getWriteableFrames(const AudioMetadata& md) const = 0;
    virtual void setPlaybackMetadata(const AudioMetadata& md) = 0;
};

class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;
    virtual const AudioMetadata& metadata() const = 0;
    virtual DecoderStatus parseMetadata() = 0;
    virtual DecoderStatus decode(unsigned frames) = 0;
    virtual DecoderStatus seek(std::uint64_t frame) = 0;        // absolute frame index
    virtual unsigned outputFrameSize() const = 0;
    virtual bool ended() const = 0;
    virtual std::uint64_t totalFrames() const = 0;
    // TCNG: drop the next `frames` decoded frames (a seek into a restarted segmented stream,
    // see IStreamSource::restartAt).
    virtual void skipFrames(std::uint64_t frames) = 0;
};

}  // namespace tcng::media
