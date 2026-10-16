#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <numeric>
#include <vector>

#include "media/stream/BandwidthDetector.hpp"
#include "media/stream/BufferStream.hpp"
#include "media/stream/Range.hpp"
#include "media/stream/StreamManager.hpp"

using namespace tcng::media;

namespace {

std::shared_ptr<const std::vector<char>> bytes(std::size_t n, char start = 0) {
    auto v = std::make_shared<std::vector<char>>(n);
    std::iota(v->begin(), v->end(), start);
    return v;
}

}  // namespace

TEST_CASE("Range is inclusive and builds the Range header") {
    const Range r{0, 1023};
    CHECK(r.size() == 1024);
    CHECK(r.isValid());
    CHECK_FALSE(Range{5, 5}.isValid());
    CHECK_FALSE(Range{6, 5}.isValid());
    CHECK(buildRangeValue(r) == "bytes=0-1023");
    CHECK(buildRangeValue({1048576, 2097151}) == "bytes=1048576-2097151");
}

TEST_CASE("Content-Range total length") {
    CHECK(getContentRangeLength("bytes 0-1023/4096") == 4096u);
    CHECK(getContentRangeLength("bytes 0-0/12345678901") == 12345678901u);
    CHECK_FALSE(getContentRangeLength("bytes 0-1023/*"));
    CHECK_FALSE(getContentRangeLength("bytes 0-1023"));
}

TEST_CASE("BufferStream: contiguous reads, gaps, end of stream") {
    BufferStream s;
    s.setSize(100);
    s.append(Buffer::make(0, bytes(40, 0)));
    s.append(Buffer::make(60, bytes(40, 60)));
    CHECK(s.availableBytes() == 40);
    CHECK_FALSE(s.complete());
    CHECK(s.nextIncompleteRange(0) == Range{40, 59});

    std::vector<char> out(100);
    CHECK(s.read(out.data(), 100) == 40);  // stops at the gap
    CHECK(s.position() == 40);
    CHECK(s.seek(50) == -1);
    CHECK(s.seek(10) == 10);

    // overlapping, 16-byte aligned fill of the gap
    s.append(Buffer::make(32, bytes(28, 32)));
    CHECK(s.availableBytes() == 90);
    CHECK(s.complete());
    CHECK(s.read(out.data(), 100) == 90);
    for (int i = 0; i < 90; ++i)
        REQUIRE(out[static_cast<std::size_t>(i)] == static_cast<char>(i + 10));
    CHECK(s.position() == 100);
    CHECK(s.endOfStream());
    s.seek(99);
    CHECK_FALSE(s.endOfStream());  // TCNG: the last byte is still readable
}

TEST_CASE("BufferStream::shrink keeps at least two buffers") {
    BufferStream s;
    s.setSize(400);
    for (int i = 0; i < 4; ++i)
        s.append(Buffer::make(static_cast<std::uint64_t>(i) * 100, bytes(100)));
    s.shrink(250);
    CHECK(s.bufferCount() == 2);
    CHECK(s.loadedBytes() == 200);
    s.shrink(1000);
    CHECK(s.bufferCount() == 2);
}

TEST_CASE("StreamManager: first range per format, then 1 MiB ranges from the gap") {
    BandwidthDetector bw;
    StreamManager flac(bw, 100 * 1024 * 1024);
    flac.load({.url = "https://cdn/x.flac", .format = "flac"});
    CHECK(flac.format() == StreamFormat::Flac);
    CHECK(flac.range() == Range{0, 512 * 1024 - 1});

    StreamManager aac(bw, 100 * 1024 * 1024);
    aac.load({.url = "https://cdn/x.mp4", .format = "aac"});
    CHECK(aac.range() == Range{0, 256 * 1024 - 1});

    StreamManager unknown(bw, 100 * 1024 * 1024);
    unknown.load({.url = "https://cdn/x", .format = "unknown"});
    CHECK(unknown.range() == Range{0, 1024 * 1024 - 1});

    StreamChunkDescriptor chunk;
    chunk.range = Range{0, 512 * 1024 - 1};
    chunk.body = bytes(512 * 1024);
    chunk.totalLength = 3 * 1024 * 1024 + 5;
    chunk.downloadMs = 100;
    flac.append(chunk);
    CHECK(flac.size() == 3 * 1024 * 1024 + 5);
    CHECK(flac.range() == Range{512 * 1024, 512 * 1024 + 1024 * 1024 - 1});

    // last range is clamped to the file size
    chunk.range = Range{512 * 1024, 3 * 1024 * 1024 - 1};
    chunk.body = bytes(chunk.range.size());
    flac.append(chunk);
    CHECK(flac.range() == Range{3 * 1024 * 1024, 3 * 1024 * 1024 + 4});
}

TEST_CASE("StreamManager: --stream-fetch-limit only frees decoded data of large files") {
    BandwidthDetector bw;
    StreamManager small(bw, 1000);
    small.load({.url = "u", .format = "flac"});
    StreamChunkDescriptor c;
    c.totalLength = 900;
    for (int i = 0; i < 3; ++i) {
        c.range = Range{static_cast<std::uint64_t>(i) * 300, static_cast<std::uint64_t>(i) * 300 + 299};
        c.body = bytes(300);
        small.append(c);
    }
    small.setDecodedPosition(800);
    CHECK(small.bufferStream().bufferCount() == 3);  // 900 < limit: no shrink

    StreamManager large(bw, 800);
    large.load({.url = "u", .format = "flac"});
    for (int i = 0; i < 3; ++i) {
        c.range = Range{static_cast<std::uint64_t>(i) * 300, static_cast<std::uint64_t>(i) * 300 + 299};
        large.append(c);
    }
    large.setDecodedPosition(800);
    CHECK(large.bufferStream().bufferCount() == 2);
}

TEST_CASE("BandwidthDetector range sizes") {
    BandwidthDetector bw;
    CHECK(bw.getRangeSize() == 1024 * 1024);
    CHECK(bw.mode() == BandwidthDetector::Mode::Uninitialized);

    bw.setDuration(200.0);              // 200 s
    bw.setFileSize(40 * 1024 * 1024);   // needs ~210 B/ms
    bw.pushNextChunk(1024 * 1024, 1000);  // ~1048 B/ms: fast
    CHECK(bw.getRangeSize() == 1024 * 1024);
    CHECK(bw.mode() == BandwidthDetector::Mode::Fast);

    BandwidthDetector slow;
    slow.setDuration(200.0);
    slow.setFileSize(40 * 1024 * 1024);
    slow.pushNextChunk(1024 * 1024, 10000);  // ~105 B/ms: slow
    const auto first = slow.getRangeSize();
    CHECK(slow.mode() == BandwidthDetector::Mode::SlowFirst);
    CHECK(first == 5 * 1024 * 1024);  // clamped to the slow-connection maximum
    CHECK(slow.getRangeSize() == 1024 * 1024);
    CHECK(slow.mode() == BandwidthDetector::Mode::SlowRest);
}

TEST_CASE("BandwidthDetector slow first range uses a 30 s prebuffer") {
    BandwidthDetector bw;
    bw.setDuration(100.0);
    bw.setFileSize(10 * 1024 * 1024);
    // 70 s downloadable at 100 B/ms = 7,000,000 bytes -> 10,485,760 - 7,000,000
    CHECK(bw.getSlowConnectionFirstRangeSize(100.0, 0.0) == 10 * 1024 * 1024 - 7000000);
}
