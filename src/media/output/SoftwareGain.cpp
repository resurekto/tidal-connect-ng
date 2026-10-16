#include "media/output/SoftwareGain.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace tcng::media {

namespace {

template <typename T>
T scaled(T v, double gain, double lo, double hi) {
    return static_cast<T>(std::clamp(std::nearbyint(static_cast<double>(v) * gain), lo, hi));
}

}  // namespace

void applyGain(void* samples, std::size_t count, SampleType type, float gain) {
    if (gain == 1.0f)
        return;
    const double g = gain;
    switch (type) {
    case SampleType::Int16: {
        auto* s = static_cast<std::int16_t*>(samples);
        for (std::size_t i = 0; i < count; ++i)
            s[i] = scaled<std::int16_t>(s[i], g, -32768.0, 32767.0);
        break;
    }
    case SampleType::Int32: {
        auto* s = static_cast<std::int32_t*>(samples);
        for (std::size_t i = 0; i < count; ++i)
            s[i] = scaled<std::int32_t>(s[i], g, -2147483648.0, 2147483647.0);
        break;
    }
    case SampleType::Float32: {
        auto* s = static_cast<float*>(samples);
        for (std::size_t i = 0; i < count; ++i)
            s[i] *= gain;
        break;
    }
    case SampleType::Int24: {
        auto* b = static_cast<std::uint8_t*>(samples);
        for (std::size_t i = 0; i < count; ++i, b += 3) {
            std::int32_t v = static_cast<std::int32_t>(static_cast<std::uint32_t>(b[0]) |
                                                       (static_cast<std::uint32_t>(b[1]) << 8) |
                                                       (static_cast<std::uint32_t>(b[2]) << 16));
            if (v & 0x800000)
                v -= 0x1000000;  // sign-extend
            v = scaled<std::int32_t>(v, g, -8388608.0, 8388607.0);
            b[0] = static_cast<std::uint8_t>(v);
            b[1] = static_cast<std::uint8_t>(v >> 8);
            b[2] = static_cast<std::uint8_t>(v >> 16);
        }
        break;
    }
    }
}

void widenWithGain(const std::int16_t* in, std::int32_t* out, std::size_t count, float gain) {
    const double g = static_cast<double>(gain) * 65536.0;
    for (std::size_t i = 0; i < count; ++i)
        out[i] = gain == 1.0f ? static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint16_t>(in[i])) << 16)
                              : scaled<std::int32_t>(static_cast<std::int32_t>(in[i]), g, -2147483648.0, 2147483647.0);
}

}  // namespace tcng::media
