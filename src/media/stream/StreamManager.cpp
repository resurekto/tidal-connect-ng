#include "media/stream/StreamManager.hpp"

#include <algorithm>

#include "util/Log.hpp"

namespace tcng::media {

std::uint64_t getFirstChunkSize(StreamFormat format) {
    if (isAacFamily(format))
        return 256 * 1024;
    if (format == StreamFormat::Flac)
        return 512 * 1024;
    return 1024 * 1024;
}

StreamManager::StreamManager(BandwidthDetector& bandwidth, std::uint64_t fetchLimit)
    : bandwidth_(bandwidth), fetchLimit_(fetchLimit) {}

void StreamManager::load(const StreamDescriptor& descriptor) {
    descriptor_ = descriptor;
}

StreamFormat StreamManager::format() const {
    if (formatOverride_ != StreamFormat::None)
        return formatOverride_;
    return streamFormatFromString(descriptor_.format);
}

Range StreamManager::range() {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    if (stream_.size() == 0) {
        end = getFirstChunkSize(format()) - 1;
    } else {
        const Range gap = stream_.nextIncompleteRange(stream_.position());
        begin = gap.begin / 16 * 16;
        end = gap.end;
    }
    end = std::min(end, begin + bandwidth_.getRangeSize() - 1);
    if (stream_.size() != 0)
        end = std::min(end, stream_.size() - 1);
    return Range{begin, end};
}

std::optional<FetchRequest> StreamManager::nextFetch() {
    const Range r = range();
    if (!r.isValid())
        return std::nullopt;
    return FetchRequest{descriptor_.url, r};
}

void StreamManager::append(const StreamChunkDescriptor& chunk) {
    Buffer buffer = Buffer::make(chunk.range.begin, chunk.body);
    if (buffer.size() != 0)
        bandwidth_.pushNextChunk(buffer.size(), chunk.downloadMs);
    if (stream_.size() == 0) {
        stream_.setSize(chunk.totalLength);
        log::get()->debug("StreamManager: stream size {}", stream_.size());
    }
    if (!buffer.range.isValid()) {
        log::get()->warn("StreamManager: dropping chunk with invalid range [{}, {}]", buffer.range.begin, buffer.range.end);
        return;
    }
    stream_.append(std::move(buffer));
}

void StreamManager::setDecodedPosition(std::uint64_t position) {
    if (position != 0 && shrinkMode())
        stream_.shrink(position);
}

}  // namespace tcng::media
