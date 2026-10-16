#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <vector>

#include "CommandLine.hpp"

using namespace tcng;

namespace {
ParseResult parse(std::vector<const char*> args, Config& c, std::string* errText = nullptr) {
    args.insert(args.begin(), "tidal-connect-ng");
    std::ostringstream out, err;
    auto r = parseCommandLine(static_cast<int>(args.size()), args.data(), c, out, err);
    if (errText)
        *errText = err.str();
    return r;
}
}  // namespace

TEST_CASE("defaults") {
    Config c;
    REQUIRE(parse({}, c) == ParseResult::Run);
    CHECK(c.websocketPort == 2019);
    CHECK(c.modelName == "TidalConnect");
    CHECK(c.friendlyName == "TidalConnect");
    CHECK(c.netifForDeviceId.empty());  // machine id instead of eth0
    CHECK(c.clientId.empty());
    CHECK_FALSE(c.logLevel.has_value());
#ifndef _WIN32
    CHECK(c.controllerSocketPath == "/tmp/tisoc-controller");
#endif
}

TEST_CASE("a typical launch line parses") {
    Config c;
    REQUIRE(parse({"--tc-certificate-path", "/x/cert.pem", "--playback-device", "default", "-f",
                   "TCNG Receiver", "--model-name", "Raspberry Pi", "--log-level", "3"},
                  c) == ParseResult::Run);
    CHECK(c.friendlyName == "TCNG Receiver");
    CHECK(c.modelName == "Raspberry Pi");
    CHECK(c.tcCertificatePath == "/x/cert.pem");
    CHECK(c.logLevel == 3);
}

TEST_CASE("dropped flags are rejected") {
    for (const char* flag : {"--codec-mqa", "--codec-mpegh", "--codec-atmos", "--disable-app-security",
                             "--disable-web-security", "--enable-websocket-log", "--enable-debug-port",
                             "--enable-mqa-passthrough"}) {
        Config c;
        std::string err;
        CHECK(parse({flag, "false"}, c, &err) == ParseResult::Error);
        CHECK(err.rfind("Error in command line: ", 0) == 0);
    }
}

TEST_CASE("range checks and their messages") {
    Config c;
    std::string err;
    CHECK(parse({"--log-level", "7"}, c, &err) == ParseResult::Error);
    CHECK(err.find("Log level outside range") != std::string::npos);
    CHECK(parse({"-p", "0"}, c, &err) == ParseResult::Error);
    CHECK(err.find("invalid port value") != std::string::npos);
    CHECK(parse({"-p", "65536"}, c, &err) == ParseResult::Error);
}

TEST_CASE("help exits through HelpShown") {
    Config c;
    CHECK(parse({"--help"}, c) == ParseResult::HelpShown);
    CHECK(parse({"-h"}, c) == ParseResult::HelpShown);
}

TEST_CASE("python-tidal switches: all paths, enabled together") {
    Config c;
    REQUIRE(parse({}, c) == ParseResult::Run);
    CHECK(c.pythonTidalPath.empty());  // off: the built-in fetch
    CHECK(c.pythonBin.empty());

    Config on;
    REQUIRE(parse({"--python-tidal-path", "/opt/python-tidal", "--python-tidal-session", "/var/lib/tc/session.json",
                   "--python-bin", "/opt/venv/bin/python"},
                  on) == ParseResult::Run);
    CHECK(on.pythonTidalPath == "/opt/python-tidal");
    CHECK(on.pythonTidalSession == "/var/lib/tc/session.json");
    CHECK(on.pythonBin == "/opt/venv/bin/python");

    Config noBin;
    REQUIRE(parse({"--python-tidal-path", "/p", "--python-tidal-session", "/s"}, noBin) == ParseResult::Run);
    CHECK(noBin.pythonBin.empty());  // python3/python from PATH

    std::string err;
    Config a;
    CHECK(parse({"--python-tidal-path", "/p"}, a, &err) == ParseResult::Error);
    CHECK(err.find("--python-tidal-session") != std::string::npos);
    Config login;
    CHECK(parse({"--python-tidal-login"}, login, &err) == ParseResult::Error);
    REQUIRE(parse({"--python-tidal-path", "/p", "--python-tidal-session", "/s", "--python-tidal-login"}, login) ==
            ParseResult::Run);
    CHECK(login.pythonTidalLogin);
    Config b;
    CHECK(parse({"--python-tidal-session", "/s"}, b) == ParseResult::Error);
    Config d;
    CHECK(parse({"--python-bin", "/usr/bin/python3"}, d) == ParseResult::Error);
}

TEST_CASE("volume switch: mixer by default, software and off accepted") {
    Config c;
    REQUIRE(parse({}, c) == ParseResult::Run);
    CHECK(c.volume == "mixer");
    for (const char* mode : {"mixer", "software", "off"}) {
        Config m;
        REQUIRE(parse({"--volume", mode}, m) == ParseResult::Run);
        CHECK(m.volume == mode);
    }
    Config bad;
    CHECK(parse({"--volume", "loud"}, bad) == ParseResult::Error);
}

TEST_CASE("mixer control switch: empty by default, passed through") {
    Config c;
    REQUIRE(parse({}, c) == ParseResult::Run);
    CHECK(c.alsaMixerControl.empty());
    Config d;
    REQUIRE(parse({"--alsa-mixer-control", "Digital"}, d) == ParseResult::Run);
    CHECK(d.alsaMixerControl == "Digital");
}
