#pragma once
// RingBuffer: fixed-capacity ring of fixed-size elements.
// Single producer / single consumer: read() and write() may run concurrently on two
// threads (atomic positions); clear() and reallocation need external exclusion.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tcng::util {

class RingBuffer {
public:
    RingBuffer() = default;
    RingBuffer(std::size_t capacity, std::size_t elementSize);

    std::size_t capacity() const { return capacity_; }
    std::size_t elementSize() const { return elementSize_; }
    std::size_t occupied() const;
    std::size_t freeSpace() const { return capacity_ - occupied(); }

    // Copy up to n elements; return the number copied (clamped to free/occupied).
    std::size_t write(const void* src, std::size_t n);
    std::size_t read(void* dst, std::size_t n);
    void clear();
    // Reallocates (contents discarded). Needs exclusion from both reader and writer.
    void reset(std::size_t capacity, std::size_t elementSize);

private:
    std::vector<std::uint8_t> data_;
    std::size_t capacity_ = 0;
    std::size_t elementSize_ = 1;
    // Monotonic element counters; index = counter % capacity.
    std::atomic<std::uint64_t> readCount_{0};
    std::atomic<std::uint64_t> writeCount_{0};
};

}  // namespace tcng::util
