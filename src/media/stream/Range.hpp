#pragma once
// Range: a byte range, and the HTTP Range header helpers of HttpStreamFetcher.

#include <cstdint>
#include <optional>
#include <string>

namespace tcng::media {

// Inclusive byte range [begin, end].
struct Range {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;

    std::uint64_t size() const { return end + 1 - begin; }
    // begin < end (a single-byte range is not valid).
    bool isValid() const { return begin < end && size() != 0; }
    bool contains(std::uint64_t p) const { return p >= begin && p <= end; }
    bool operator==(const Range&) const = default;
};

// "bytes=<begin>-<end>"
std::string buildRangeValue(const Range& range);
// Total length from a Content-Range value ("bytes 0-1023/4096" -> 4096); nullopt if absent
// or not numeric (e.g. "*").
std::optional<std::uint64_t> getContentRangeLength(const std::string& contentRange);

}  // namespace tcng::media
