#pragma once
// Stream formats, decoder status codes and the metadata of a decoded stream.

#include <cstdint>
#include <string>

namespace tcng::media {

// Stream formats. Value 6 was MQA, which is not supported.
enum class StreamFormat : int { None = 0, Aac = 1, Mp4a40_2 = 2, AacPlus = 3, Mp4a40_5 = 4, Flac = 5 };

std::string stringify(StreamFormat format);           // "none" if unknown
StreamFormat streamFormatFromString(const std::string& name);  // None if unknown
inline bool isAacFamily(StreamFormat f) { return f >= StreamFormat::Aac && f <= StreamFormat::Mp4a40_5; }

enum class DecoderStatus : int {
    Undefined = 0,
    Success = 1,
    CorruptedMetadata = 2,
    CorruptedStream = 3,
    Underrun = 4,  // more input needed
    Complete = 5,  // end of stream
};
const char* toString(DecoderStatus status);
inline bool isError(DecoderStatus s) { return s == DecoderStatus::CorruptedMetadata || s == DecoderStatus::CorruptedStream; }

// AudioMetadata::Packaging; 5 = unset/invalid.
enum class Packaging : int {
    P16BitsIn16BitsContainer = 0,
    P16BitsIn32BitsContainer = 1,
    P24BitsIn24BitsContainer = 2,
    P24BitsIn32BitsContainerLow = 3,
    P24BitsIn32BitsContainerHigh = 4,
    Invalid = 5,
};
// Bytes per sample for a packaging: 1,3,4 -> 4; 2 -> 3; else 2 (setPlaybackMetadata).
unsigned bytesPerSample(Packaging packaging);

struct AudioMetadata {
    unsigned channels = 0;
    unsigned bitsPerSample = 0;
    unsigned sampleRatePlayback = 0;
    unsigned sampleRateOriginal = 0;  // unused
    bool interleaved = true;
    Packaging packaging = Packaging::Invalid;

    void clear() { *this = AudioMetadata{}; }
    bool valid() const {
        return channels && bitsPerSample && sampleRatePlayback && packaging != Packaging::Invalid;
    }
    // Compares bits, playback rate, original rate, channels (not interleaved/packaging).
    bool operator==(const AudioMetadata& o) const {
        return bitsPerSample == o.bitsPerSample && sampleRatePlayback == o.sampleRatePlayback &&
               sampleRateOriginal == o.sampleRateOriginal && channels == o.channels;
    }
    bool operator!=(const AudioMetadata& o) const { return !(*this == o); }
    unsigned frameBytes() const { return channels * bytesPerSample(packaging); }
};

}  // namespace tcng::media
