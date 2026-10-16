#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "spkcon/SpkconPacket.hpp"

using namespace tcng::spkcon;

namespace {
std::string bytes(std::initializer_list<int> b) {
    std::string s;
    for (int v : b)
        s.push_back(static_cast<char>(v));
    return s;
}
}  // namespace

TEST_CASE("encodeFrame produces FF 02 len payload FF 03") {
    auto f = encodeFrame("{}");
    REQUIRE(f);
    CHECK(*f == bytes({0xFF, 0x02, 0x00, 0x02, '{', '}', 0xFF, 0x03}));

    auto big = encodeFrame(std::string(300, 'x'));
    REQUIRE(big);
    CHECK(static_cast<unsigned char>((*big)[2]) == 0x01);
    CHECK(static_cast<unsigned char>((*big)[3]) == 0x2C);
    CHECK(big->size() == 306);
}

TEST_CASE("encodeFrame refuses payloads over 64 KiB") {
    CHECK_FALSE(encodeFrame(std::string(kMaxPayload + 1, 'x')));
    CHECK(encodeFrame(std::string(kMaxPayload, 'x')));
}

TEST_CASE("FrameDecoder handles split and back-to-back frames") {
    std::vector<std::string> got;
    FrameDecoder d([&](std::string p) { got.push_back(std::move(p)); });
    const std::string stream = *encodeFrame("{\"a\":1}") + *encodeFrame("{\"b\":2}");
    for (char c : stream)
        d.feed(std::string_view(&c, 1));
    REQUIRE(got.size() == 2);
    CHECK(got[0] == "{\"a\":1}");
    CHECK(got[1] == "{\"b\":2}");

    got.clear();
    d.feed(stream);
    CHECK(got.size() == 2);
}

TEST_CASE("FrameDecoder accepts zero-length frames") {
    std::vector<std::string> got;
    FrameDecoder d([&](std::string p) { got.push_back(std::move(p)); });
    d.feed(bytes({0xFF, 0x02, 0x00, 0x00, 0xFF, 0x03}) + *encodeFrame("x"));
    REQUIRE(got.size() == 2);
    CHECK(got[0].empty());
    CHECK(got[1] == "x");
}

TEST_CASE("FrameDecoder resynchronises after garbage in the same read") {
    std::vector<std::string> got;
    int errors = 0;
    FrameDecoder d([&](std::string p) { got.push_back(std::move(p)); }, [&](const std::string&) { ++errors; });
    d.feed("junk" + *encodeFrame("ok"));
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "ok");
    CHECK(errors > 0);
}
