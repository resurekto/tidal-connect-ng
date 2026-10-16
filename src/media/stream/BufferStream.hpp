#pragma once
// BufferStream: the downloaded byte ranges of one track, read sequentially by the decoder
// through a position cursor. Loop-thread only.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "media/stream/Range.hpp"

namespace tcng::media {

struct Buffer {
    Range range;
    std::shared_ptr<const std::vector<char>> data;

    // setData(begin, data): range [begin, begin + len - 1].
    static Buffer make(std::uint64_t begin, std::shared_ptr<const std::vector<char>> data);
    std::uint64_t size() const { return data ? data->size() : 0; }
};

class BufferStream {
public:
    void append(Buffer buffer);   // push_back + sort by range
    void clear();
    void setSize(std::uint64_t size);  // ignored when 0
    std::uint64_t size() const { return size_; }
    std::uint64_t position() const { return position_; }

    // position = p; returns p if a buffer contains it, else -1.
    std::int64_t seek(std::uint64_t p);
    // size && pos >= size: not one byte early, or the last FLAC frame fails its CRC.
    bool endOfStream() const;
    std::uint64_t availableBytes() const;   // contiguous bytes from position
    std::size_t read(char* dst, std::size_t n);  // stops at the first gap
    bool complete() const;                  // size && size - pos <= availableBytes()
    // First gap at/after p: [forward from p, backward from size-1].
    Range nextIncompleteRange(std::uint64_t p) const;
    // Drops leading buffers that end before p, keeping at least two.
    void shrink(std::uint64_t p);
    std::uint64_t loadedBytes() const;
    std::size_t bufferCount() const { return buffers_.size(); }

private:
    const Buffer* buffer(std::uint64_t p) const;

    std::vector<Buffer> buffers_;
    std::uint64_t size_ = 0;
    std::uint64_t position_ = 0;
};

}  // namespace tcng::media
