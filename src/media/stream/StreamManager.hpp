#pragma once
// StreamManager: the IInputStream the decoders read from, backed by a BufferStream of
// downloaded ranges. Decides the next range to request and frees decoded buffers for large
// files.
//
// --stream-fetch-limit: not a download cap. When the file size is >= the limit, buffers
// behind the decoder's position are released (setDecodedPosition, FLAC only).

#include <cstdint>
#include <string>

#include "media/audio/Interfaces.hpp"
#include "media/stream/BandwidthDetector.hpp"
#include "media/stream/BufferStream.hpp"
#include "media/stream/HttpStreamFetcher.hpp"
#include "media/stream/IStreamSource.hpp"

namespace tcng::media {

// The first range to request: AAC 256 KiB, FLAC 512 KiB, otherwise 1 MiB.
std::uint64_t getFirstChunkSize(StreamFormat format);

class StreamManager : public IStreamSource {
public:
    StreamManager(BandwidthDetector& bandwidth, std::uint64_t fetchLimit);

    // IStreamSource
    void load(const StreamDescriptor& descriptor) override;
    const std::string& url() const override { return descriptor_.url; }
    // range() of the url; nullopt when the range is not valid.
    std::optional<FetchRequest> nextFetch() override;
    void append(const StreamChunkDescriptor& chunk) override;

    // Next range to request.
    Range range();
    bool shrinkMode() const { return stream_.size() >= fetchLimit_; }
    const BufferStream& bufferStream() const { return stream_; }

    // IInputStream
    void setDecodedPosition(std::uint64_t position) override;
    void setStreamFormat(StreamFormat format) override { formatOverride_ = format; }
    StreamFormat format() const override;
    bool endOfStream() const override { return stream_.endOfStream(); }
    bool complete() const override { return stream_.complete(); }
    std::uint64_t availableBytes() const override { return stream_.availableBytes(); }
    std::uint64_t position() const override { return stream_.position(); }
    std::uint64_t size() const override { return stream_.size(); }
    std::size_t read(char* dst, std::size_t n) override { return stream_.read(dst, n); }
    std::int64_t seek(std::uint64_t position) override { return stream_.seek(position); }

private:
    BandwidthDetector& bandwidth_;
    const std::uint64_t fetchLimit_;
    BufferStream stream_;
    StreamDescriptor descriptor_;
    StreamFormat formatOverride_ = StreamFormat::None;
};

}  // namespace tcng::media
