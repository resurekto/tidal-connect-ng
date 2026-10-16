#pragma once
// TCNG (--volume software): sample scaling for PortSink's callback. A gain of exactly 1 leaves
// the samples untouched (bit-perfect). 16-bit sources are widened to 32 bits first, so a low
// volume does not throw away their resolution.

#include <cstddef>
#include <cstdint>

#include "media/output/IPcmSink.hpp"

namespace tcng::media {

// In place: `count` samples (frames * channels) of `type`. Int24 is packed little endian.
void applyGain(void* samples, std::size_t count, SampleType type, float gain);
// 16-bit -> 32-bit (left-aligned), scaled.
void widenWithGain(const std::int16_t* in, std::int32_t* out, std::size_t count, float gain);

}  // namespace tcng::media
