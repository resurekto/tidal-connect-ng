#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <memory>
#include <vector>

#include <FLAC++/encoder.h>

#include "media/audio/AudioDecoderFactory.hpp"
#include "media/audio/AudioOutputBuffer.hpp"
#include "media/audio/FLACDecoder.hpp"
#include "media/stream/StreamManager.hpp"

using namespace tcng::media;

namespace {

class MemoryEncoder : public FLAC::Encoder::Stream {
public:
    std::vector<char> data;

protected:
    // Seekable, so the encoder rewrites STREAMINFO (total samples, min/max frame size).
    ::FLAC__StreamEncoderWriteStatus write_callback(const FLAC__byte buffer[], size_t bytes, uint32_t, uint32_t) override {
        if (pos + bytes > data.size())
            data.resize(pos + bytes);
        std::copy(buffer, buffer + bytes, data.begin() + static_cast<long>(pos));
        pos += bytes;
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    }
    ::FLAC__StreamEncoderSeekStatus seek_callback(FLAC__uint64 offset) override {
        pos = static_cast<std::size_t>(offset);
        return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
    }
    ::FLAC__StreamEncoderTellStatus tell_callback(FLAC__uint64* offset) override {
        *offset = pos;
        return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
    }
    std::size_t pos = 0;
};

// Encodes `frames` frames of a deterministic signal; returns the file and the samples.
std::vector<char> encode(unsigned channels, unsigned bits, unsigned rate, unsigned frames,
                         std::vector<std::int32_t>& samples) {
    MemoryEncoder enc;
    enc.set_channels(channels);
    enc.set_bits_per_sample(bits);
    enc.set_sample_rate(rate);
    enc.set_compression_level(5);
    enc.set_total_samples_estimate(frames);
    REQUIRE(enc.init() == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
    const double amp = std::pow(2.0, bits - 1) * 0.5;
    samples.resize(static_cast<std::size_t>(frames) * channels);
    for (unsigned i = 0; i < frames; ++i)
        for (unsigned c = 0; c < channels; ++c)
            samples[i * channels + c] =
                static_cast<std::int32_t>(amp * std::sin(2 * std::numbers::pi * (440.0 + 110.0 * c) * i / rate));
    REQUIRE(enc.process_interleaved(samples.data(), frames));
    REQUIRE(enc.finish());
    return enc.data;
}

struct Fixture {
    BandwidthDetector bw;
    StreamManager stream{bw, 100 * 1024 * 1024};
    AudioOutputBuffer output{1 << 20};

    explicit Fixture(const std::vector<char>& file, std::size_t firstChunk) {
        stream.load({.url = "mem://x", .format = "unknown"});
        StreamChunkDescriptor c;
        c.range = Range{0, firstChunk - 1};
        c.body = std::make_shared<std::vector<char>>(file.begin(), file.begin() + static_cast<long>(firstChunk));
        c.totalLength = file.size();
        stream.append(c);
        if (firstChunk < file.size()) {
            rest.range = Range{firstChunk, file.size() - 1};
            rest.body = std::make_shared<std::vector<char>>(file.begin() + static_cast<long>(firstChunk), file.end());
            rest.totalLength = file.size();
        }
    }
    StreamChunkDescriptor rest;
};

std::vector<std::uint8_t> drain(AudioOutputBuffer& out, std::size_t frameBytes) {
    std::vector<std::uint8_t> pcm;
    std::vector<std::uint8_t> buf(4096 * frameBytes);
    for (;;) {
        const std::size_t n = out.readInterleavedPcmData(buf.data(), 4096);
        if (n == 0)
            break;
        pcm.insert(pcm.end(), buf.begin(), buf.begin() + static_cast<long>(n * frameBytes));
    }
    return pcm;
}

}  // namespace

TEST_CASE("FLAC: probe, metadata and full decode of a 16-bit stereo stream") {
    std::vector<std::int32_t> samples;
    const unsigned frames = 44100;  // 1 s
    const auto file = encode(2, 16, 44100, frames, samples);
    Fixture fx(file, file.size());
    AudioDecoderFactory factory;
    CHECK(AudioDecoderFactory::probeStream(fx.stream) == StreamFormat::Flac);
    auto decoder = factory.create(fx.stream, fx.output);
    REQUIRE(decoder);
    REQUIRE(decoder->parseMetadata() == DecoderStatus::Success);
    const AudioMetadata md = decoder->metadata();
    CHECK(md.valid());
    CHECK(md.channels == 2);
    CHECK(md.bitsPerSample == 16);
    CHECK(md.sampleRatePlayback == 44100);
    CHECK(decoder->totalFrames() == frames);
    fx.output.setPlaybackMetadata(md);

    std::vector<std::uint8_t> pcm;
    for (int guard = 0; guard < 1000 && !decoder->ended(); ++guard) {
        const auto st = decoder->decode(8820);
        REQUIRE_FALSE(isError(st));
        auto chunk = drain(fx.output, 4);
        pcm.insert(pcm.end(), chunk.begin(), chunk.end());
        if (st == DecoderStatus::Complete)
            break;
    }
    CHECK(decoder->ended());
    REQUIRE(pcm.size() == static_cast<std::size_t>(frames) * 4);  // including the last frame
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto v = static_cast<std::int16_t>(pcm[i * 2] | (pcm[i * 2 + 1] << 8));
        REQUIRE(v == samples[i]);
    }
}

TEST_CASE("FLAC: 24-bit mono is duplicated to packed 24-bit stereo") {
    std::vector<std::int32_t> samples;
    const auto file = encode(1, 24, 96000, 9600, samples);
    Fixture fx(file, file.size());
    FLACDecoder decoder(fx.stream, fx.output);
    REQUIRE(decoder.parseMetadata() == DecoderStatus::Success);
    CHECK(decoder.metadata().bitsPerSample == 24);
    CHECK(decoder.metadata().packaging == Packaging::P24BitsIn24BitsContainer);
    fx.output.setPlaybackMetadata(decoder.metadata());
    std::vector<std::uint8_t> pcm;
    while (!decoder.ended()) {
        const auto st = decoder.decode(19200);
        REQUIRE_FALSE(isError(st));
        auto c = drain(fx.output, 6);
        pcm.insert(pcm.end(), c.begin(), c.end());
        if (st == DecoderStatus::Complete)
            break;
    }
    REQUIRE(pcm.size() == 9600u * 6);
    for (std::size_t i = 0; i < 9600; ++i) {
        const std::uint8_t* f = &pcm[i * 6];
        std::int32_t l = f[0] | (f[1] << 8) | (f[2] << 16);
        std::int32_t r = f[3] | (f[4] << 8) | (f[5] << 16);
        l = (l << 8) >> 8;
        r = (r << 8) >> 8;
        REQUIRE(l == samples[i]);
        REQUIRE(r == samples[i]);
    }
}

TEST_CASE("FLAC: decoding stops with Underrun until more data arrives, then resumes") {
    std::vector<std::int32_t> samples;
    const auto file = encode(2, 16, 44100, 44100 * 3, samples);
    Fixture fx(file, file.size() / 3);
    FLACDecoder decoder(fx.stream, fx.output);
    REQUIRE(decoder.parseMetadata() == DecoderStatus::Success);
    fx.output.setPlaybackMetadata(decoder.metadata());
    std::size_t total = 0;
    DecoderStatus st = DecoderStatus::Success;
    for (int i = 0; i < 100 && st != DecoderStatus::Underrun; ++i) {
        st = decoder.decode(8820);
        total += drain(fx.output, 4).size() / 4;
    }
    CHECK(st == DecoderStatus::Underrun);
    CHECK(total > 0);
    CHECK(total < 44100u * 3);
    fx.stream.append(fx.rest);
    for (int i = 0; i < 1000 && !decoder.ended(); ++i) {
        st = decoder.decode(8820);
        REQUIRE_FALSE(isError(st));
        total += drain(fx.output, 4).size() / 4;
    }
    CHECK(total == 44100u * 3);
}

TEST_CASE("FLAC: seek to an absolute frame") {
    std::vector<std::int32_t> samples;
    const auto file = encode(2, 16, 44100, 44100 * 2, samples);
    Fixture fx(file, file.size());
    FLACDecoder decoder(fx.stream, fx.output);
    REQUIRE(decoder.parseMetadata() == DecoderStatus::Success);
    fx.output.setPlaybackMetadata(decoder.metadata());
    REQUIRE(decoder.seek(44100) == DecoderStatus::Success);
    std::size_t total = 0;
    std::vector<std::uint8_t> first;
    for (int i = 0; i < 1000 && !decoder.ended(); ++i) {
        const auto st = decoder.decode(8820);
        auto c = drain(fx.output, 4);
        if (first.empty())
            first = c;
        total += c.size() / 4;
        if (st == DecoderStatus::Complete)
            break;
    }
    CHECK(total == 44100u);
    REQUIRE(first.size() >= 4);
    CHECK(static_cast<std::int16_t>(first[0] | (first[1] << 8)) == samples[44100 * 2]);
}

TEST_CASE("decoder factory probes non-FLAC data as AAC") {
    BandwidthDetector bw;
    StreamManager stream(bw, 1000);
    stream.load({.url = "u", .format = "unknown"});
    StreamChunkDescriptor c;
    c.range = Range{0, 15};
    c.body = std::make_shared<std::vector<char>>(std::vector<char>{0, 0, 0, 0x20, 'f', 't', 'y', 'p', 0, 0, 0, 0, 0, 0, 0, 0});
    c.totalLength = 16;
    stream.append(c);
    CHECK(AudioDecoderFactory::probeStream(stream) == StreamFormat::Aac);
    CHECK(stream.position() == 0);
}
