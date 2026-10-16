#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "TidalConnect.hpp"
#include "media/PythonTidalFetchTask.hpp"
#include "media/TidalUrlFetchTask.hpp"
#include "test_util/Unique.hpp"
#include "test_util/TestCert.hpp"
#include "test_util/WsClient.hpp"

using namespace std::chrono_literals;
using Json = nlohmann::json;

namespace {

// Next message from the phone socket with the given command. Messages from different
// services may arrive in any order, so skipped ones are kept for later calls.
Json receiveCommand(tcng::test::WsClient& phone, const std::string& command) {
    static std::vector<Json> backlog;
    for (auto it = backlog.begin(); it != backlog.end(); ++it) {
        if (it->value("command", "") == command) {
            Json j = *it;
            backlog.erase(it);
            return j;
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto text = phone.receive(1s);
        if (text.empty())
            continue;
        auto j = Json::parse(text);
        if (j.value("command", "") == command)
            return j;
        backlog.push_back(std::move(j));
    }
    return {};
}

}  // namespace

TEST_CASE("end to end: a phone starts a session, sets volume, loads a queue and ends") {
    const auto dir = std::filesystem::temp_directory_path();
    tcng::test::TempPem pem{"tcng-app"};
    REQUIRE(pem.ok);

    tcng::Config config;
    config.tcCertificatePath = pem.path;
    config.websocketPort = tcng::test::uniquePort(2);
    config.controllerSocketPath = tcng::test::uniqueSocketPath("tcng-app");
    config.clientId = "test-client";
    config.friendlyName = "TCNG test";
    config.volume = "off";  // never touch the machine's real output volume
    tcng::TidalConnect app(config);
    app.start();

    tcng::test::WsClient phone(config.websocketPort);
    REQUIRE(phone.waitConnected());

    phone.send(R"({"command":"startSession","appId":"tidal","appName":"TIDAL","sessionCredential":"cred"})");
    const Json started = receiveCommand(phone, "notifySessionStarted");
    REQUIRE(started.is_object());
    CHECK(started["joined"] == false);
    const std::string sessionId = started["sessionId"];

    // Casting start pushes the current volume (0.2) first; then our change arrives.
    CHECK(receiveCommand(phone, "notifyDeviceStatusChanged")["volume"]["level"] == 0.2);
    phone.send(R"({"command":"setVolume","requestId":5,"level":0.4})");
    const Json status = receiveCommand(phone, "notifyDeviceStatusChanged");
    CHECK(status["volume"]["level"] == 0.4);
    const Json result = receiveCommand(phone, "notifyRequestResult");
    CHECK(result["requestId"] == 5);
    CHECK(result["resultCode"] == 0);

    // A local queue whose only item has an unreachable URL: the queue loads, playback
    // reports the media and then a playback error.
    phone.send(Json{{"command", "loadLocalQueue"},
                    {"requestId", 6},
                    {"queueInfo", {{"queueId", "q1"}, {"repeatMode", "OFF"}, {"shuffled", false}}},
                    {"currentMediaInfo",
                     {{"itemId", "i1"}, {"srcUrl", "http://127.0.0.1:1/x.flac"}, {"metadata", {{"title", "t"}}}}},
                    {"position", 0},
                    {"autoplay", false}}
                   .dump());
    const Json media = receiveCommand(phone, "notifyMediaChanged");
    CHECK(media["mediaInfo"]["itemId"] == "i1");
    const Json request = receiveCommand(phone, "requestQueueItems");
    REQUIRE(request.is_object());
    phone.send(Json{{"command", "respondQueueItems"},
                    {"requestId", 7},
                    {"queueRequestId", request["queueRequestId"]},
                    {"mediaInfos", Json::array({{{"itemId", "i1"}, {"srcUrl", "http://127.0.0.1:1/x.flac"},
                                                 {"metadata", {{"title", "t"}}}}})},
                    {"hasBegin", true},
                    {"hasEnd", true}}
                   .dump());
    CHECK(receiveCommand(phone, "notifyRequestResult").is_object());

    phone.send(Json{{"command", "endSession"}, {"sessionId", sessionId}, {"stopCasting", true}}.dump());
    const Json ended = receiveCommand(phone, "notifySessionEnded");
    CHECK(ended["suspended"] == false);
    CHECK(phone.waitClosed());

    app.stop();
}

TEST_CASE("python-tidal: the fetch task factory follows the config") {
    using tcng::Config;
    using tcng::TidalConnect;
    namespace fs = std::filesystem;
    Config off;
    CHECK(dynamic_cast<const tcng::media::TidalUrlFetchTaskFactory*>(TidalConnect::makeFetchTaskFactory(off).get()));

    const fs::path dir = fs::temp_directory_path() / ("tcng-app-pytidal-" + std::to_string(tcng::test::uniquePort()));
    fs::create_directories(dir / "checkout");
    std::ofstream(dir / "session.json") << "{}";
    Config on;
    on.pythonTidalPath = (dir / "checkout").string();
    on.pythonTidalSession = (dir / "session.json").string();
    if (tcng::media::PythonTidalFetchTask::resolvePython(""))
        CHECK(dynamic_cast<const tcng::media::PythonTidalFetchTaskFactory*>(TidalConnect::makeFetchTaskFactory(on).get()));

    Config badDir = on;
    badDir.pythonTidalPath = (dir / "missing").string();
    CHECK_THROWS_WITH(TidalConnect::makeFetchTaskFactory(badDir), Catch::Matchers::ContainsSubstring("not a directory"));
    Config badSession = on;
    badSession.pythonTidalSession = (dir / "missing.json").string();
    CHECK_THROWS_WITH(TidalConnect::makeFetchTaskFactory(badSession), Catch::Matchers::ContainsSubstring("login"));
    Config badBin = on;
    badBin.pythonBin = (dir / "no-python").string();
    CHECK_THROWS_WITH(TidalConnect::makeFetchTaskFactory(badBin), Catch::Matchers::ContainsSubstring("--python-bin"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}
