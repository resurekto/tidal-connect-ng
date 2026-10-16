#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "media/audio/AudioOutputBuffer.hpp"
#include "media/audio/Formats.hpp"
#include "util/RingBuffer.hpp"

using namespace tcng::media;

TEST_CASE("tcng::util::RingBuffer clamps and wraps") {
    tcng::util::RingBuffer rb(4, 2);
    const std::uint16_t in[6] = {1, 2, 3, 4, 5, 6};
    CHECK(rb.write(in, 3) == 3);
    CHECK(rb.occupied() == 3);
    CHECK(rb.write(in + 3, 3) == 1);  // only one slot left
    std::uint16_t out[4] = {};
    CHECK(rb.read(out, 2) == 2);
    CHECK(out[0] == 1);
    CHECK(out[1] == 2);
    CHECK(rb.write(in + 4, 2) == 2);  // wraps
    CHECK(rb.read(out, 10) == 4);
    CHECK(std::vector<std::uint16_t>(out, out + 4) == std::vector<std::uint16_t>{3, 4, 5, 6});
    CHECK(rb.occupied() == 0);
    rb.write(in, 2);
    rb.clear();
    CHECK(rb.occupied() == 0);
}

TEST_CASE("tcng::util::RingBuffer SPSC across threads keeps order") {
    tcng::util::RingBuffer rb(1000, sizeof(std::uint32_t));
    constexpr std::uint32_t kCount = 200000;
    std::thread producer([&] {
        std::uint32_t next = 0;
        while (next < kCount) {
            if (rb.write(&next, 1) == 1)
                ++next;
        }
    });
    std::uint32_t expected = 0;
    bool ordered = true;
    while (expected < kCount) {
        std::uint32_t v;
        if (rb.read(&v, 1) == 1) {
            ordered = ordered && v == expected;
            ++expected;
        }
    }
    producer.join();
    CHECK(ordered);
}

TEST_CASE("AudioMetadata equality and validity") {
    AudioMetadata a;
    CHECK_FALSE(a.valid());
    a.channels = 2;
    a.bitsPerSample = 16;
    a.sampleRatePlayback = 44100;
    a.packaging = Packaging::P16BitsIn16BitsContainer;
    CHECK(a.valid());
    AudioMetadata b = a;
    b.packaging = Packaging::P24BitsIn24BitsContainer;  // ignored by ==
    CHECK(a == b);
    b.sampleRatePlayback = 48000;
    CHECK(a != b);
    CHECK(bytesPerSample(Packaging::P24BitsIn24BitsContainer) == 3);
    CHECK(bytesPerSample(Packaging::P24BitsIn32BitsContainerLow) == 4);
    CHECK(bytesPerSample(Packaging::P16BitsIn16BitsContainer) == 2);
    CHECK(stringify(StreamFormat::Mp4a40_5) == "mp4a.40.5");
    CHECK(streamFormatFromString("flac") == StreamFormat::Flac);
    CHECK(streamFormatFromString("unknown") == StreamFormat::None);
}

TEST_CASE("AudioOutputBuffer sizes the ring from the metadata and calls back") {
    AudioOutputBuffer buf(100);  // frames per channel
    AudioMetadata md;
    md.channels = 2;
    md.bitsPerSample = 24;
    md.sampleRatePlayback = 96000;
    md.packaging = Packaging::P24BitsIn24BitsContainer;
    int calls = 0;
    buf.setPlaybackMetadataCallback([&](const AudioMetadata& m) {
        ++calls;
        CHECK(m == md);
    });
    buf.setPlaybackMetadata(md);
    CHECK(calls == 1);
    CHECK(buf.getWriteableFrames(md) == 100);
    std::vector<std::uint8_t> frames(150 * 6, 0x5a);
    CHECK(buf.appendInterleaved(frames.data(), 150, md) == 100);  // short write: dropped
    CHECK(buf.framesAvailableForRead() == 100);
    CHECK(buf.getFillGrade() == 1.0);
    std::vector<std::uint8_t> out(40 * 6);
    CHECK(buf.readInterleavedPcmData(out.data(), 40) == 40);
    CHECK(out[0] == 0x5a);
    CHECK(buf.framesAvailableForRead() == 60);
    buf.flush();
    CHECK(buf.framesAvailableForRead() == 0);
}
