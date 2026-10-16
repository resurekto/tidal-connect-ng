#include "media/stream/Mp4Flac.hpp"

#include <algorithm>
#include <cstdint>

namespace tcng::media {

namespace {

constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[3]));
}

std::uint64_t be(const std::uint8_t* p, int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i)
        v = (v << 8) | p[i];
    return v;
}

struct Box {
    std::uint32_t type = 0;
    const std::uint8_t* payload = nullptr;
    std::size_t size = 0;
};

// Reads the box at p and advances p past it; false at the end or on a malformed size.
bool nextBox(const std::uint8_t*& p, const std::uint8_t* end, Box& box) {
    const auto left = static_cast<std::uint64_t>(end - p);
    if (left < 8)
        return false;
    std::uint64_t size = be(p, 4);
    std::uint64_t header = 8;
    if (size == 1) {
        if (left < 16)
            return false;
        size = be(p + 8, 8);
        header = 16;
    } else if (size == 0) {
        size = left;  // to the end of the data
    }
    if (size < header || size > left)
        return false;
    box.type = static_cast<std::uint32_t>(be(p + 4, 4));
    box.payload = p + header;
    box.size = static_cast<std::size_t>(size - header);
    p += size;
    return true;
}

// Depth-first search for dfLa along moov/trak/mdia/minf/stbl/stsd/fLaC.
std::optional<Box> findDfLa(const std::uint8_t* p, const std::uint8_t* end, int depth) {
    if (depth > 8)
        return std::nullopt;
    Box box;
    while (nextBox(p, end, box)) {
        std::size_t skip = 0;
        switch (box.type) {
        case fourcc("dfLa"):
            return box;
        case fourcc("moov"):
        case fourcc("trak"):
        case fourcc("mdia"):
        case fourcc("minf"):
        case fourcc("stbl"):
            break;
        case fourcc("stsd"):
            skip = 8;  // version/flags, entry_count
            break;
        case fourcc("fLaC"):
            skip = 28;  // AudioSampleEntry fields before the child boxes
            break;
        default:
            continue;
        }
        if (box.size < skip)
            continue;
        if (auto found = findDfLa(box.payload + skip, box.payload + box.size, depth + 1))
            return found;
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::vector<char>> headerFromInit(const char* data, std::size_t size) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(data);
    const auto dfla = findDfLa(begin, begin + size, 0);
    if (!dfla || dfla->size < 4 + 4)
        return std::nullopt;
    // FullBox version/flags, then FLAC METADATA_BLOCKs (STREAMINFO first).
    const std::uint8_t* blocks = dfla->payload + 4;
    const std::uint8_t* end = dfla->payload + dfla->size;
    if ((blocks[0] & 0x7f) != 0)  // not STREAMINFO
        return std::nullopt;
    std::vector<char> out{'f', 'L', 'a', 'C'};
    std::size_t lastHeader = 0;
    for (const std::uint8_t* p = blocks; p < end;) {
        if (end - p < 4)
            return std::nullopt;
        const auto length = static_cast<std::size_t>(be(p + 1, 3));
        if (static_cast<std::size_t>(end - p - 4) < length)
            return std::nullopt;
        const bool last = (p[0] & 0x80) != 0;
        lastHeader = out.size();
        out.insert(out.end(), p, p + 4 + length);
        p += 4 + length;
        if (last)
            break;
    }
    out[lastHeader] = static_cast<char>(static_cast<std::uint8_t>(out[lastHeader]) | 0x80);
    return out;
}

std::vector<char> framesFromSegment(const char* data, std::size_t size) {
    std::vector<char> out;
    MdatStream stream;
    stream.feed(data, size, out);
    return out;
}

void MdatStream::feed(const char* data, std::size_t size, std::vector<char>& out) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(data);
    const auto* end = p + size;
    while (p < end && !failed_) {
        if (inBox_) {
            const auto left = static_cast<std::uint64_t>(end - p);
            const auto n = static_cast<std::size_t>(toEnd_ ? left : std::min(remaining_, left));
            if (mdat_)
                out.insert(out.end(), p, p + n);
            p += n;
            if (!toEnd_ && (remaining_ -= n) == 0)
                inBox_ = false;
            continue;
        }
        // Box header: 8 bytes, or 16 with a 64-bit size.
        const std::size_t need = header_.size() >= 4 && be(header_.data(), 4) == 1 ? 16 : 8;
        const auto take = std::min<std::size_t>(need - header_.size(), static_cast<std::size_t>(end - p));
        header_.insert(header_.end(), p, p + take);
        p += take;
        if (header_.size() < 8 || (be(header_.data(), 4) == 1 && header_.size() < 16))
            continue;
        std::uint64_t boxSize = be(header_.data(), 4);
        const std::uint64_t headerSize = boxSize == 1 ? 16 : 8;
        if (boxSize == 1)
            boxSize = be(header_.data() + 8, 8);
        mdat_ = static_cast<std::uint32_t>(be(header_.data() + 4, 4)) == fourcc("mdat");
        toEnd_ = boxSize == 0;
        if (!toEnd_ && boxSize < headerSize) {
            failed_ = true;
            break;
        }
        remaining_ = toEnd_ ? 0 : boxSize - headerSize;
        inBox_ = toEnd_ || remaining_ != 0;
        header_.clear();
    }
}

}  // namespace tcng::media
