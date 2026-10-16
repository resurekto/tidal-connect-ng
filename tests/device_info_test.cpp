#include <catch2/catch_test_macros.hpp>

#include "core/DeviceInfo.hpp"

using tcng::core::DeviceInfo;

TEST_CASE("md5Hex is lowercase hex") {
    CHECK(DeviceInfo::md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(DeviceInfo::md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
}

TEST_CASE("formatMac: lowercase, colon-separated") {
    const std::uint8_t mac[6] = {0x02, 0xAB, 0xCD, 0x01, 0x0A, 0xFF};
    CHECK(DeviceInfo::formatMac(mac) == "02:ab:cd:01:0a:ff");
}

TEST_CASE("device id follows md5hex(md5hex(tisoc+mac+cosit)+seed)") {
    const std::string base = DeviceInfo::md5Hex("tisoc02:ab:cd:01:0a:ffcosit");
    CHECK(base.size() == 32);
    CHECK(DeviceInfo::md5Hex(base + "seed").size() == 32);
}

TEST_CASE("unknown interface falls back to the illegal hash, still double-hashed") {
    const auto id = DeviceInfo::deviceId("no-such-interface-xyz", "");
    CHECK(id == DeviceInfo::md5Hex(DeviceInfo::ILLEGAL_HASH_VALUE));
}

TEST_CASE("machine id is available on this host and stable") {
    REQUIRE(DeviceInfo::machineId().has_value());
    const auto a = DeviceInfo::deviceId("", "");
    CHECK(a == DeviceInfo::deviceId("", ""));
    CHECK(a != DeviceInfo::deviceId("", "other-seed"));
    CHECK(a.size() == 32);
}
