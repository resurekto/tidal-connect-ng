#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include <FLAC++/encoder.h>

#include "media/audio/AudioDecoderFactory.hpp"
#include "media/audio/AudioOutputBuffer.hpp"
#include "media/stream/BandwidthDetector.hpp"
#include "media/stream/Mp4Flac.hpp"
#include "media/stream/SegmentStreamManager.hpp"
#include "media/stream/StreamManager.hpp"
#include "FlacPackage.hpp"

using namespace tcng::media;
using namespace tcng::test;

namespace {

// Answers one request of `s` like a CDN: 206 + Content-Range for a range, 200 otherwise.
StreamChunkDescriptor respond(const FetchRequest& req, const std::string& body, bool honourRanges = true,
                              bool sendTotal = true) {
    auto c = chunk(req.url, body);
    c.httpStatus = 200;
    if (req.range && honourRanges) {
        const auto begin = static_cast<std::size_t>(req.range->begin);
        const auto end = std::min<std::size_t>(static_cast<std::size_t>(req.range->end), body.size() - 1);
        c = chunk(req.url, begin < body.size() ? body.substr(begin, end - begin + 1) : std::string());
        c.httpStatus = 206;
        c.range = *req.range;
        c.totalLength = sendTotal ? body.size() : 0;
    }
    return c;
}

// The package body a request is for.
const std::string& bodyFor(const Package& p, const std::string& url) {
    if (url == p.descriptor.initUrl)
        return p.bodies[0];
    for (std::size_t i = 0; i < p.descriptor.segments.size(); ++i)
        if (p.descriptor.segments[i].url == url)
            return p.bodies[i + 1];
    FAIL("unknown url " << url);
    return p.bodies[0];
}

// Feeds every outstanding download of `s` from the package until nothing is missing.
// Returns the requests made.
std::vector<FetchRequest> downloadAll(SegmentStreamManager& s, const Package& p, bool honourRanges = true,
                                      bool sendTotal = true) {
    std::vector<FetchRequest> made;
    for (int guard = 0; guard < 10000; ++guard) {
        auto req = s.nextFetch();
        if (!req)
            break;
        made.push_back(*req);
        s.append(respond(*req, bodyFor(p, req->url), honourRanges, sendTotal));
    }
    return made;
}

std::vector<std::int16_t> decodeAll(IAudioDecoder& decoder, AudioOutputBuffer& output, std::uint64_t skip = 0) {
    REQUIRE(decoder.parseMetadata() == DecoderStatus::Success);
    output.setPlaybackMetadata(decoder.metadata());
    if (skip)
        decoder.skipFrames(skip);
    std::vector<std::int16_t> pcm;
    std::vector<std::int16_t> buf(4096 * 2);
    for (int guard = 0; guard < 10000; ++guard) {
        const auto st = decoder.decode(4410);
        for (std::size_t n; (n = output.readInterleavedPcmData(buf.data(), 4096)) != 0;)
            pcm.insert(pcm.end(), buf.begin(), buf.begin() + static_cast<long>(n * 2));
        if (st == DecoderStatus::Complete || decoder.ended() || isError(st))
            break;
    }
    return pcm;
}

}  // namespace

TEST_CASE("mp4flac: dfLa becomes the native FLAC header, mdat payloads the frames") {
    const auto e = encode(16, 44100, 44100);
    const std::string init = initSegment(e);
    const auto header = headerFromInit(init.data(), init.size());
    REQUIRE(header);
    CHECK(*header == std::vector<char>(e.file.begin(), e.file.begin() + static_cast<long>(e.firstFrame)));

    const std::string seg = mediaSegment(e, 0, 3);
    const auto frames = framesFromSegment(seg.data(), seg.size());
    const std::size_t bytes = e.frames[0].second + e.frames[1].second + e.frames[2].second;
    CHECK(frames == std::vector<char>(e.file.begin() + static_cast<long>(e.firstFrame),
                                      e.file.begin() + static_cast<long>(e.firstFrame + bytes)));

    CHECK_FALSE(headerFromInit(seg.data(), seg.size()));        // no dfLa
    CHECK_FALSE(headerFromInit(init.data(), init.size() / 2));  // truncated
    const std::string noMdat = box("moof", "x");
    CHECK(framesFromSegment(noMdat.data(), noMdat.size()).empty());
}

TEST_CASE("mp4flac: the last metadata block gets the last-block flag") {
    const auto e = encode(16, 44100, 8192);
    std::string dfla;
    be32(dfla, 0);
    std::string blocks(e.file.data() + 4, e.firstFrame - 4);
    std::size_t last = 0;  // header offset of the last block
    for (std::size_t at = 0; at + 4 <= blocks.size();) {
        last = at;
        at += 4 + ((static_cast<unsigned char>(blocks[at + 1]) << 16) | (static_cast<unsigned char>(blocks[at + 2]) << 8) |
                   static_cast<unsigned char>(blocks[at + 3]));
    }
    REQUIRE((static_cast<unsigned char>(blocks[last]) & 0x80) != 0);
    blocks[last] = static_cast<char>(blocks[last] & 0x7f);  // clear it
    dfla += blocks;
    std::string stsd;
    be32(stsd, 0);
    be32(stsd, 1);
    stsd += box("fLaC", std::string(28, '\0') + box("dfLa", dfla));
    const std::string init = box("moov", box("trak", box("mdia", box("minf", box("stbl", box("stsd", stsd))))));
    const auto header = headerFromInit(init.data(), init.size());
    REQUIRE(header);
    CHECK((static_cast<unsigned char>((*header)[4 + last]) & 0x80) != 0);
    CHECK(*header == std::vector<char>(e.file.begin(), e.file.begin() + static_cast<long>(e.firstFrame)));
}

TEST_CASE("makeStreamSource: segments select SegmentStreamManager") {
    BandwidthDetector bw;
    StreamDescriptor plain{.url = "u", .format = "flac"};
    CHECK(dynamic_cast<StreamManager*>(makeStreamSource(plain, bw, 1000).get()));
    StreamDescriptor segmented{.url = "i", .format = "flac", .initUrl = "i", .segments = {{"s1", 1.0}}};
    CHECK(dynamic_cast<SegmentStreamManager*>(makeStreamSource(segmented, bw, 1000).get()));
}

TEST_CASE("SegmentStreamManager: init then segments in order; the size is known at the end") {
    const unsigned rate = 96000;
    const auto e = encode(24, rate, rate * 2);  // 2 s, 24-bit
    const auto p = package(e, rate, 8);
    SegmentStreamManager s;
    s.load(p.descriptor);
    CHECK(s.format() == StreamFormat::Flac);
    CHECK(s.durationHint() == 0.0);

    auto req = s.nextFetch();
    REQUIRE(req);
    CHECK(req->url == "https://cdn/0.mp4");
    s.append(chunk("https://cdn/9.mp4", "stale"));  // not the outstanding download
    CHECK(s.availableBytes() == 0);
    s.append(chunk(req->url, p.bodies[0]));
    CHECK(s.availableBytes() == e.firstFrame);
    CHECK(s.nextFetch()->url == "https://cdn/1.mp4");
    CHECK(s.size() == 0);

    downloadAll(s, p);
    CHECK_FALSE(s.nextFetch());
    CHECK(s.size() == e.file.size());  // header + every frame: the original file's bytes
    CHECK(s.complete());

    // The unwrapped stream is the original FLAC file.
    std::vector<char> all(e.file.size());
    s.seek(0);
    CHECK(s.read(all.data(), all.size()) == all.size());
    CHECK(all == e.file);
}

TEST_CASE("SegmentStreamManager + FLACDecoder: 24-bit metadata, restartAt") {
    const unsigned rate = 48000;
    const unsigned frames = rate * 3;
    const auto e = encode(24, rate, frames);
    const auto p = package(e, rate, 6);  // 6 x 4096 samples = 0.512 s per segment
    AudioDecoderFactory factory;

    SECTION("full decode keeps 24 bits") {
        SegmentStreamManager s;
        s.load(p.descriptor);
        downloadAll(s, p);
        s.seek(0);
        AudioOutputBuffer output(1 << 20);
        auto decoder = factory.create(s, output);
        REQUIRE(decoder);
        REQUIRE(decoder->parseMetadata() == DecoderStatus::Success);
        CHECK(decoder->metadata().bitsPerSample == 24);
        CHECK(decoder->metadata().sampleRatePlayback == rate);
    }

    SECTION("restartAt picks the segment holding the time") {
        SegmentStreamManager s;
        s.load(p.descriptor);
        CHECK(s.restartAt(0.0) == 0.0);
        const double seg = 6 * 4096.0 / rate;
        const auto start = s.restartAt(seg * 2 + 0.1);
        REQUIRE(start);
        CHECK(std::abs(*start - seg * 2) < 1e-9);
        CHECK(s.nextFetch()->url == "https://cdn/0.mp4");  // the header again
        s.append(chunk("https://cdn/0.mp4", p.bodies[0]));
        CHECK(s.nextFetch()->url == "https://cdn/3.mp4");
        REQUIRE(s.restartAt(1e9));  // past the end: the last segment
        CHECK(s.nextSegment() == p.descriptor.segments.size() - 1);
    }
}

TEST_CASE("SegmentStreamManager + FLACDecoder: skip after restart, exact samples (16-bit)") {
    const unsigned rate = 44100;
    const unsigned frames = rate * 3;
    const auto e = encode(16, rate, frames);
    const auto p = package(e, rate, 5);
    SegmentStreamManager s;
    s.load(p.descriptor);
    const double target = 1.7;
    const auto start = s.restartAt(target);
    REQUIRE(start);
    REQUIRE(*start <= target);
    downloadAll(s, p);
    s.seek(0);
    AudioOutputBuffer output(1 << 22);
    AudioDecoderFactory factory;
    auto decoder = factory.create(s, output);
    REQUIRE(decoder);
    const auto skip = static_cast<std::uint64_t>((target - *start) * rate);
    const auto pcm = decodeAll(*decoder, output, skip);
    const auto first = static_cast<std::size_t>(*start * rate + 0.5) + skip;
    REQUIRE(pcm.size() == (frames - first) * 2);
    CHECK(pcm[0] == e.samples[first * 2]);
    CHECK(pcm[1] == e.samples[first * 2 + 1]);
    CHECK(pcm.back() == e.samples.back());
}

TEST_CASE("SegmentStreamManager: prefetch up to kSegmentsAhead, decoded segments are released") {
    const unsigned rate = 44100;
    const auto e = encode(16, rate, rate * 4);
    const auto p = package(e, rate, 2);
    SegmentStreamManager s;
    s.load(p.descriptor);
    CHECK_FALSE(s.wantsPrefetch());  // the header comes first, on demand
    s.append(chunk(s.nextFetch()->url, p.bodies[0]));
    for (std::size_t i = 0; i <= SegmentStreamManager::kSegmentsAhead; ++i) {
        CHECK(s.wantsPrefetch());
        s.append(chunk(s.nextFetch()->url, p.bodies[i + 1]));
    }
    CHECK_FALSE(s.wantsPrefetch());  // 3 unread segments buffered
    const std::size_t buffers = s.bufferStream().bufferCount();
    // The decoder moves past the first two segments.
    std::vector<char> sink(s.availableBytes());
    const std::uint64_t secondEnd = e.firstFrame + e.frames[0].second + e.frames[1].second + e.frames[2].second +
                                    e.frames[3].second;
    s.read(sink.data(), static_cast<std::size_t>(secondEnd));
    s.setDecodedPosition(s.position());
    CHECK(s.bufferStream().bufferCount() < buffers);
    CHECK(s.wantsPrefetch());
}

TEST_CASE("mp4flac: MdatStream fed in any pieces equals framesFromSegment") {
    const auto e = encode(16, 44100, 44100);
    const std::string seg = box("styp", "msdh") + box("moof", std::string(100, 'm')) +
                            box("mdat", std::string(e.file.data() + e.firstFrame, e.file.size() - e.firstFrame)) +
                            box("free", "xx");
    const auto whole = framesFromSegment(seg.data(), seg.size());
    REQUIRE(whole.size() == e.file.size() - e.firstFrame);
    for (std::size_t piece : {1u, 3u, 7u, 8u, 9u, 4096u}) {
        MdatStream m;
        std::vector<char> out;
        for (std::size_t at = 0; at < seg.size(); at += piece)
            m.feed(seg.data() + at, std::min(piece, seg.size() - at), out);
        CHECK(out == whole);
        CHECK_FALSE(m.failed());
    }
    // 64-bit box size
    std::string big;
    be32(big, 1);
    big += "mdat";
    be32(big, 0);
    be32(big, 16 + 5);
    big += "hello";
    CHECK(framesFromSegment(big.data(), big.size()) == std::vector<char>{'h', 'e', 'l', 'l', 'o'});
    // size 0: to the end
    std::string open;
    be32(open, 0);
    open += "mdat";
    open += "abc";
    CHECK(framesFromSegment(open.data(), open.size()) == std::vector<char>{'a', 'b', 'c'});
}

TEST_CASE("SegmentStreamManager: a small first range after load and restart, the header is kept") {
    const unsigned rate = 44100;
    const auto e = encode(16, rate, rate * 20);
    const auto p = package(e, rate, 60);  // ~5.6 s, > 64 KB per segment
    REQUIRE(p.bodies[1].size() > SegmentStreamManager::kFirstRangeBytes);
    SegmentStreamManager s;
    s.load(p.descriptor);

    auto made = downloadAll(s, p);
    REQUIRE(made.size() >= 4);
    CHECK(made[0].url == p.descriptor.initUrl);  // init: whole
    CHECK_FALSE(made[0].range);
    REQUIRE(made[1].range);  // first segment: 64 KB, then the rest
    CHECK(*made[1].range == Range{0, SegmentStreamManager::kFirstRangeBytes - 1});
    REQUIRE(made[2].range);
    CHECK(made[2].range->begin == SegmentStreamManager::kFirstRangeBytes);
    CHECK(made[2].range->end == p.bodies[1].size() - 1);
    CHECK_FALSE(made[3].range);  // later segments: whole
    std::vector<char> all(e.file.size());
    s.seek(0);
    REQUIRE(s.size() == e.file.size());
    CHECK(s.read(all.data(), all.size()) == all.size());
    CHECK(all == e.file);

    // A restart: no init download, a small range of the target segment first.
    REQUIRE(s.restartAt(2.0));
    CHECK(s.availableBytes() == e.firstFrame);  // the kept header
    made = downloadAll(s, p);
    REQUIRE_FALSE(made.empty());
    CHECK(made[0].url != p.descriptor.initUrl);
    REQUIRE(made[0].range);
    CHECK(made[0].range->begin == 0);
    s.seek(0);
    AudioOutputBuffer output(1 << 22);
    AudioDecoderFactory factory;
    auto decoder = factory.create(s, output);
    REQUIRE(decoder);
    CHECK(decoder->parseMetadata() == DecoderStatus::Success);
}

TEST_CASE("SegmentStreamManager: decoding can start after the first range") {
    const unsigned rate = 44100;
    const auto e = encode(16, rate, rate * 20);
    const auto p = package(e, rate, 60);  // ~5.6 s, > 64 KB per segment
    SegmentStreamManager s;
    s.load(p.descriptor);
    s.append(respond(*s.nextFetch(), p.bodies[0]));  // init
    const auto first = s.nextFetch();
    REQUIRE(first->range);
    s.append(respond(*first, p.bodies[1]));
    // header + the FLAC frames of the first 64 KB (minus the moof)
    CHECK(s.availableBytes() > e.firstFrame + SegmentStreamManager::kFirstRangeBytes / 2);
    CHECK(s.nextSegment() == 0);  // still in progress
    s.seek(0);
    AudioOutputBuffer output(1 << 22);
    AudioDecoderFactory factory;
    auto decoder = factory.create(s, output);
    REQUIRE(decoder);
    REQUIRE(decoder->parseMetadata() == DecoderStatus::Success);
    output.setPlaybackMetadata(decoder->metadata());
    CHECK(decoder->decode(4096) == DecoderStatus::Success);
}

TEST_CASE("SegmentStreamManager: servers that ignore ranges or omit the total") {
    const unsigned rate = 44100;
    const auto e = encode(16, rate, rate * 4);
    const auto p = package(e, rate, 10);
    for (const bool honour : {false, true}) {
        SegmentStreamManager s;
        s.load(p.descriptor);
        downloadAll(s, p, honour, /*sendTotal=*/honour ? false : true);
        std::vector<char> all(e.file.size());
        s.seek(0);
        REQUIRE(s.size() == e.file.size());
        CHECK(s.read(all.data(), all.size()) == all.size());
        CHECK(all == e.file);
    }
}
