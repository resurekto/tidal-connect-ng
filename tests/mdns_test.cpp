#include <catch2/catch_test_macros.hpp>

#include <dns_sd.h>

#include <string>

#include "core/MdnsServer.hpp"

using tcng::core::MdnsServer;

namespace {
std::string value(const std::string& txt, const char* key) {
    uint8_t len = 0;
    const void* v = TXTRecordGetValuePtr(static_cast<uint16_t>(txt.size()), txt.data(), key, &len);
    return v ? std::string(static_cast<const char*>(v), len) : std::string("<missing>");
}
}  // namespace

TEST_CASE("TXT record carries mn, ca, id, fn and ve=1") {
    MdnsServer::Config c;
    c.modelName = "Raspberry Pi";
    c.friendlyName = "TCNG Receiver";
    c.deviceId = std::string(32, 'a');
    const auto txt = MdnsServer::txtRecord(c);
    CHECK(value(txt, "mn") == "Raspberry Pi");
    CHECK(value(txt, "ca") == "0");
    CHECK(value(txt, "id") == std::string(32, 'a'));
    CHECK(value(txt, "fn") == "TCNG Receiver");
    CHECK(value(txt, "ve") == "1");
    CHECK(TXTRecordGetCount(static_cast<uint16_t>(txt.size()), txt.data()) == 5);
}

TEST_CASE("setConfig rejects fields of 256 bytes or more") {
    MdnsServer server;
    MdnsServer::Config c;
    c.modelName = "m";
    c.friendlyName = std::string(256, 'x');
    CHECK_FALSE(server.setConfig(c));
    c.friendlyName = "ok";
    CHECK(server.setConfig(c));
}
