#include "media/stream/BufferStream.hpp"

#include <algorithm>
#include <cstring>

namespace tcng::media {

Buffer Buffer::make(std::uint64_t begin, std::shared_ptr<const std::vector<char>> data) {
    Buffer b;
    const std::uint64_t len = data ? data->size() : 0;
    b.range = Range{begin, len ? begin + len - 1 : begin};
    b.data = std::move(data);
    return b;
}

void BufferStream::append(Buffer buffer) {
    buffers_.push_back(std::move(buffer));
    std::sort(buffers_.begin(), buffers_.end(), [](const Buffer& a, const Buffer& b) {
        return a.range.begin < b.range.begin || (a.range.begin == b.range.begin && a.range.end < b.range.end);
    });
}

void BufferStream::clear() {
    size_ = 0;
    position_ = 0;
    buffers_.clear();
}

void BufferStream::setSize(std::uint64_t size) {
    if (size != 0)
        size_ = size;
}

const Buffer* BufferStream::buffer(std::uint64_t p) const {
    for (const auto& b : buffers_)
        if (b.size() && b.range.contains(p))
            return &b;
    return nullptr;
}

std::int64_t BufferStream::seek(std::uint64_t p) {
    position_ = p;
    return buffer(p) ? static_cast<std::int64_t>(p) : -1;
}

bool BufferStream::endOfStream() const {
    return size_ != 0 && position_ >= size_;
}

std::uint64_t BufferStream::availableBytes() const {
    const Buffer* b = buffer(position_);
    if (!b)
        return 0;
    // Ranges can overlap (requests start 16-byte aligned), so count up to the end of the
    // contiguous run rather than adding whole buffer sizes.
    std::uint64_t end = b->range.end;
    while (const Buffer* next = buffer(end + 1))
        end = next->range.end;
    return end + 1 - position_;
}

std::size_t BufferStream::read(char* dst, std::size_t n) {
    std::size_t done = 0;
    while (done < n) {
        const Buffer* b = buffer(position_);
        if (!b)
            break;
        const std::uint64_t offset = position_ - b->range.begin;
        const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(n - done, b->size() - offset));
        std::memcpy(dst + done, b->data->data() + offset, chunk);
        done += chunk;
        position_ += chunk;
    }
    return done;
}

bool BufferStream::complete() const {
    if (size_ == 0)
        return false;
    if (position_ >= size_)
        return true;
    return size_ - position_ <= availableBytes();
}

Range BufferStream::nextIncompleteRange(std::uint64_t p) const {
    std::uint64_t end = size_ ? size_ - 1 : 0;
    std::uint64_t begin = p;
    while (const Buffer* b = buffer(begin)) {
        begin = b->range.end + 1;
        if (begin >= end)
            break;
    }
    while (const Buffer* b = buffer(end)) {
        if (b->range.begin == 0)
            break;
        end = b->range.begin - 1;
        if (end <= begin)
            break;
    }
    return Range{begin, end};
}

void BufferStream::shrink(std::uint64_t p) {
    while (buffers_.size() > 2 && buffers_.front().range.end < p)
        buffers_.erase(buffers_.begin());
}

std::uint64_t BufferStream::loadedBytes() const {
    std::uint64_t total = 0;
    for (const auto& b : buffers_)
        total += b.size();
    return total;
}

}  // namespace tcng::media
