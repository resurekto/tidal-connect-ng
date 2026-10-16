#pragma once
// Test helpers: FLAC encoded in memory with its frame boundaries, packaged as DASH-style
// fragmented MP4 (init segment with dfLa + one moof/mdat per media segment).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include <FLAC++/encoder.h>

#include "media/stream/HttpStreamFetcher.hpp"
#include "media/stream/IStreamSource.hpp"

namespace tcng::test {

using namespace tcng::media;

// A FLAC file plus the byte range of every frame, as a DASH packager would see it.
struct EncodedFlac {
    std::vector<char> file;
    std::size_t firstFrame = 0;  // offset of the first frame (end of the metadata)
    std::vector<std::pair<std::size_t, std::size_t>> frames;  // offset, size
    std::vector<std::int32_t> samples;                         // interleaved
};

class FrameRecorder : public FLAC::Encoder::Stream {
public:
    EncodedFlac* out = nullptr;

protected:
    ::FLAC__StreamEncoderWriteStatus write_callback(const FLAC__byte buffer[], size_t bytes, uint32_t samples,
                                                    uint32_t) override {
        if (samples > 0)
            out->frames.emplace_back(pos, bytes);
        if (pos + bytes > out->file.size())
            out->file.resize(pos + bytes);
        std::copy(buffer, buffer + bytes, out->file.begin() + static_cast<long>(pos));
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

inline EncodedFlac encode(unsigned bits, unsigned rate, unsigned frames) {
    EncodedFlac e;
    FrameRecorder enc;
    enc.out = &e;
    enc.set_channels(2);
    enc.set_bits_per_sample(bits);
    enc.set_sample_rate(rate);
    enc.set_blocksize(4096);
    enc.set_total_samples_estimate(frames);
    REQUIRE(enc.init() == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
    const double amp = std::pow(2.0, bits - 1) * 0.5;
    e.samples.resize(static_cast<std::size_t>(frames) * 2);
    for (unsigned i = 0; i < frames; ++i)
        for (unsigned c = 0; c < 2; ++c)
            e.samples[i * 2 + c] =
                static_cast<std::int32_t>(amp * std::sin(2 * std::numbers::pi * (440.0 + 110.0 * c) * i / rate));
    REQUIRE(enc.process_interleaved(e.samples.data(), frames));
    REQUIRE(enc.finish());
    REQUIRE_FALSE(e.frames.empty());
    e.firstFrame = e.frames.front().first;
    return e;
}

inline void be32(std::string& s, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
        s.push_back(static_cast<char>((v >> shift) & 0xff));
}

inline std::string box(const char* type, const std::string& payload) {
    std::string s;
    be32(s, static_cast<std::uint32_t>(8 + payload.size()));
    s.append(type, 4);
    return s + payload;
}

inline std::string initSegment(const EncodedFlac& e) {
    std::string dfla;
    be32(dfla, 0);  // version/flags
    dfla.append(e.file.data() + 4, e.firstFrame - 4);  // metadata blocks after "fLaC"
    std::string stsd;
    be32(stsd, 0);
    be32(stsd, 1);
    stsd += box("fLaC", std::string(28, '\0') + box("dfLa", dfla));
    return box("ftyp", "iso6") +
           box("moov", box("mvhd", std::string(100, '\0')) +
                           box("trak", box("mdia", box("minf", box("stbl", box("stsd", stsd))))));
}

inline std::string mediaSegment(const EncodedFlac& e, std::size_t firstFrame, std::size_t count) {
    std::string mdat;
    for (std::size_t i = firstFrame; i < firstFrame + count && i < e.frames.size(); ++i)
        mdat.append(e.file.data() + e.frames[i].first, e.frames[i].second);
    return box("styp", "msdh") + box("moof", box("mfhd", std::string(8, '\0'))) + box("mdat", mdat);
}

inline StreamChunkDescriptor chunk(const std::string& url, const std::string& body) {
    StreamChunkDescriptor c;
    c.url = url;
    c.body = std::make_shared<std::vector<char>>(body.begin(), body.end());
    c.totalLength = body.size();
    return c;
}

// DASH-style package of `e`: framesPerSegment FLAC frames (4096 samples each) per segment.
struct Package {
    StreamDescriptor descriptor;
    std::vector<std::string> bodies;  // [0] = init, then the media segments
};

inline Package package(const EncodedFlac& e, unsigned rate, std::size_t framesPerSegment) {
    Package p;
    p.descriptor.format = "flac";
    p.descriptor.initUrl = "https://cdn/0.mp4";
    p.descriptor.url = p.descriptor.initUrl;
    p.bodies.push_back(initSegment(e));
    for (std::size_t f = 0, n = 1; f < e.frames.size(); f += framesPerSegment, ++n) {
        const std::size_t count = std::min(framesPerSegment, e.frames.size() - f);
        p.descriptor.segments.push_back({"https://cdn/" + std::to_string(n) + ".mp4", count * 4096.0 / rate});
        p.bodies.push_back(mediaSegment(e, f, count));
    }
    return p;
}

}  // namespace tcng::test
