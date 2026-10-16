#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <vector>

#include "media/output/IVolumeControl.hpp"
#include "media/output/SoftwareGain.hpp"
#include "media/output/VolumeCurve.hpp"

using namespace tcng::media;
using Catch::Matchers::WithinAbs;

TEST_CASE("volume curve: alsamixer's cubic dB mapping and its inverse") {
    CHECK_THAT(sliderToDb(0.0, -103.5, 0.0), WithinAbs(-103.5, 1e-9));
    CHECK_THAT(sliderToDb(1.0, -103.5, 0.0), WithinAbs(0.0, 1e-9));
    // Half the slider is about -17.5 dB (60*log10(0.5)+...), not -52 dB as a linear map gives.
    const double half = sliderToDb(0.5, -103.5, 0.0);
    CHECK(half > -20.0);
    CHECK(half < -15.0);
    for (double v : {0.0, 0.1, 0.25, 0.5, 0.8, 1.0})
        CHECK_THAT(dbToSlider(sliderToDb(v, -60.0, 6.0), -60.0, 6.0), WithinAbs(v, 1e-9));
    double last = -1000;
    for (int i = 0; i <= 100; ++i) {  // monotonic
        const double db = sliderToDb(i / 100.0, -80.0, 0.0);
        CHECK(db > last - 1e-12);
        last = db;
    }
}

TEST_CASE("volume curve: software gain is silent at 0 and unity at 1") {
    CHECK(sliderToGain(0.0) == 0.0);
    CHECK_THAT(sliderToGain(1.0), WithinAbs(1.0, 1e-12));
    // cubic: (0.5 * 0.9 + 0.1)^3 with a 60 dB floor
    CHECK_THAT(sliderToGain(0.5), WithinAbs(0.166375, 1e-6));
    CHECK_THAT(sliderToGain(1e-9), WithinAbs(0.001, 1e-6));  // -60 dB
}

TEST_CASE("software gain: unity is bit-exact, formats are scaled and clamped") {
    std::vector<std::int16_t> s16{1000, -1000, 32767, -32768};
    auto copy = s16;
    applyGain(s16.data(), s16.size(), SampleType::Int16, 1.0f);
    CHECK(s16 == copy);
    applyGain(s16.data(), s16.size(), SampleType::Int16, 0.5f);
    CHECK(s16 == std::vector<std::int16_t>{500, -500, 16384, -16384});

    std::vector<std::int32_t> s32{1 << 30, -(1 << 30)};
    applyGain(s32.data(), s32.size(), SampleType::Int32, 0.25f);
    CHECK(s32 == std::vector<std::int32_t>{1 << 28, -(1 << 28)});

    // packed little-endian 24-bit: +4000000, -4000000
    auto pack = [](std::int32_t v, std::uint8_t* b) {
        b[0] = static_cast<std::uint8_t>(v);
        b[1] = static_cast<std::uint8_t>(v >> 8);
        b[2] = static_cast<std::uint8_t>(v >> 16);
    };
    auto unpack = [](const std::uint8_t* b) {
        std::int32_t v = b[0] | (b[1] << 8) | (b[2] << 16);
        return (v & 0x800000) ? v - 0x1000000 : v;
    };
    std::uint8_t s24[6];
    pack(4000000, s24);
    pack(-4000000, s24 + 3);
    applyGain(s24, 2, SampleType::Int24, 0.5f);
    CHECK(unpack(s24) == 2000000);
    CHECK(unpack(s24 + 3) == -2000000);
}

TEST_CASE("software gain: 16-bit is widened to 32-bit, exact at unity") {
    const std::vector<std::int16_t> in{1, -1, 32767, -32768};
    std::vector<std::int32_t> out(in.size());
    widenWithGain(in.data(), out.data(), in.size(), 1.0f);
    CHECK(out == std::vector<std::int32_t>{65536, -65536, 32767 * 65536, -32768 * 65536});
    widenWithGain(in.data(), out.data(), in.size(), 0.5f);
    CHECK(out[0] == 32768);  // half of a 16-bit LSB survives in 32 bits
    CHECK(out[1] == -32768);
}

TEST_CASE("volume mode names") {
    CHECK(parseVolumeMode("mixer") == VolumeMode::Mixer);
    CHECK(parseVolumeMode("software") == VolumeMode::Software);
    CHECK(parseVolumeMode("off") == VolumeMode::Off);
    CHECK_FALSE(parseVolumeMode("hardware"));
}
