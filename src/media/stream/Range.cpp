#include "media/stream/Range.hpp"

#include <charconv>

namespace tcng::media {

std::string buildRangeValue(const Range& range) {
    return "bytes=" + std::to_string(range.begin) + "-" + std::to_string(range.end);
}

std::optional<std::uint64_t> getContentRangeLength(const std::string& contentRange) {
    const auto slash = contentRange.find('/');
    if (slash == std::string::npos)
        return std::nullopt;
    std::size_t begin = slash + 1;
    while (begin < contentRange.size() && contentRange[begin] == ' ')
        ++begin;
    std::uint64_t value = 0;
    const char* first = contentRange.data() + begin;
    const char* last = contentRange.data() + contentRange.size();
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc() || ptr == first)
        return std::nullopt;
    return value;
}

}  // namespace tcng::media
