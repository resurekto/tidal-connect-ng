#include "util/RingBuffer.hpp"

#include <algorithm>
#include <cstring>

namespace tcng::util {

RingBuffer::RingBuffer(std::size_t capacity, std::size_t elementSize)
    : data_(capacity * elementSize), capacity_(capacity), elementSize_(elementSize) {}

std::size_t RingBuffer::occupied() const {
    return static_cast<std::size_t>(writeCount_.load(std::memory_order_acquire) -
                                    readCount_.load(std::memory_order_acquire));
}

std::size_t RingBuffer::write(const void* src, std::size_t n) {
    if (capacity_ == 0)
        return 0;
    const std::uint64_t w = writeCount_.load(std::memory_order_relaxed);
    const std::uint64_t r = readCount_.load(std::memory_order_acquire);
    n = std::min<std::size_t>(n, capacity_ - static_cast<std::size_t>(w - r));
    const std::size_t start = static_cast<std::size_t>(w % capacity_);
    const std::size_t first = std::min(n, capacity_ - start);
    const auto* in = static_cast<const std::uint8_t*>(src);
    std::memcpy(data_.data() + start * elementSize_, in, first * elementSize_);
    std::memcpy(data_.data(), in + first * elementSize_, (n - first) * elementSize_);
    writeCount_.store(w + n, std::memory_order_release);
    return n;
}

std::size_t RingBuffer::read(void* dst, std::size_t n) {
    if (capacity_ == 0)
        return 0;
    const std::uint64_t r = readCount_.load(std::memory_order_relaxed);
    const std::uint64_t w = writeCount_.load(std::memory_order_acquire);
    n = std::min<std::size_t>(n, static_cast<std::size_t>(w - r));
    const std::size_t start = static_cast<std::size_t>(r % capacity_);
    const std::size_t first = std::min(n, capacity_ - start);
    auto* out = static_cast<std::uint8_t*>(dst);
    std::memcpy(out, data_.data() + start * elementSize_, first * elementSize_);
    std::memcpy(out + first * elementSize_, data_.data(), (n - first) * elementSize_);
    readCount_.store(r + n, std::memory_order_release);
    return n;
}

void RingBuffer::reset(std::size_t capacity, std::size_t elementSize) {
    data_.assign(capacity * elementSize, 0);
    data_.shrink_to_fit();
    capacity_ = capacity;
    elementSize_ = elementSize;
    readCount_ = 0;
    writeCount_ = 0;
}

void RingBuffer::clear() {
    readCount_.store(writeCount_.load());
}

}  // namespace tcng::util
