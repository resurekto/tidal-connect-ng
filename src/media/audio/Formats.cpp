#include "media/audio/Formats.hpp"

#include <array>
#include <utility>

namespace tcng::media {

namespace {

constexpr std::array<std::pair<StreamFormat, const char*>, 6> kFormats{{
    {StreamFormat::None, "none"},
    {StreamFormat::Aac, "aac"},
    {StreamFormat::Mp4a40_2, "mp4a.40.2"},
    {StreamFormat::AacPlus, "aac+"},
    {StreamFormat::Mp4a40_5, "mp4a.40.5"},
    {StreamFormat::Flac, "flac"},
}};

}  // namespace

std::string stringify(StreamFormat format) {
    for (const auto& [f, name] : kFormats)
        if (f == format)
            return name;
    return "none";
}

StreamFormat streamFormatFromString(const std::string& name) {
    for (const auto& [f, n] : kFormats)
        if (name == n)
            return f;
    return StreamFormat::None;
}

const char* toString(DecoderStatus status) {
    switch (status) {
    case DecoderStatus::Undefined: return "Undefined";
    case DecoderStatus::Success: return "Success";
    case DecoderStatus::CorruptedMetadata: return "CorruptedMetadata";
    case DecoderStatus::CorruptedStream: return "CorruptedStream";
    case DecoderStatus::Underrun: return "Underrun";
    case DecoderStatus::Complete: return "Complete";
    }
    return "Undefined";
}

unsigned bytesPerSample(Packaging packaging) {
    switch (packaging) {
    case Packaging::P16BitsIn32BitsContainer:
    case Packaging::P24BitsIn32BitsContainerLow:
    case Packaging::P24BitsIn32BitsContainerHigh:
        return 4;
    case Packaging::P24BitsIn24BitsContainer:
        return 3;
    default:
        return 2;
    }
}

}  // namespace tcng::media
