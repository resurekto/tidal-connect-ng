#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <vector>

#include "core/ClientCommunicator.hpp"
#include "core/MessageIds.hpp"
#include "net/Loop.hpp"
#include "test_util/Unique.hpp"
#include "test_util/TestCert.hpp"
#include "test_util/WsClient.hpp"

using namespace std::chrono_literals;
using namespace tcng;
using core::Json;

namespace {

class Recorder : public core::Service {
public:
    explicit Recorder(std::string name) : Service(std::move(name)) {}
    bool waitFor(std::uint32_t id, Json* out = nullptr) {
        std::unique_lock l(m);
        return cv.wait_for(l, 3s, [&] {
            for (auto it = got.begin(); it != got.end(); ++it)
                if (it->first == id) {
                    if (out)
                        *out = it->second;
                    got.erase(it);
                    return true;
                }
            return false;
        });
    }
    bool received(std::uint32_t id) {
        std::lock_guard l(m);
        for (auto& [mid, j] : got)
            if (mid == id)
                return true;
        return false;
    }

protected:
    void handleMessage(std::uint32_t id, const Json& j) override {
        std::lock_guard l(m);
        got.emplace_back(id, j);
        cv.notify_all();
    }

private:
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::pair<std::uint32_t, Json>> got;
};

const std::uint16_t kPort = test::uniquePort(1);

struct Fixture {
    test::TempPem pem{"tcng-cc"};
    net::Loop loop;
    std::shared_ptr<core::ServiceManager> manager = std::make_shared<core::ServiceManager>();
    std::shared_ptr<Recorder> session = std::make_shared<Recorder>(core::service::session);
    std::shared_ptr<Recorder> playback = std::make_shared<Recorder>(core::service::playback);
    std::shared_ptr<core::ClientCommunicator> clientcomm;

    Fixture() {
        REQUIRE(pem.ok);
        clientcomm = std::make_shared<core::ClientCommunicator>(loop, core::ClientCommunicator::Config{kPort, pem.path});
        manager->registerService(clientcomm);
        manager->registerService(playback);
        manager->registerService(session);
        loop.start();
        manager->startAll();
    }
    ~Fixture() {
        manager->finishAll();
        loop.stop();
    }
};

}  // namespace

TEST_CASE("new sockets are blocked except for session commands") {
    Fixture f;
    test::WsClient phone(kPort);
    REQUIRE(phone.waitConnected());

    phone.send(R"({"command":"play","requestId":1})");
    phone.send(R"({"command":"startSession","appId":"tidal","appName":"TIDAL","sessionCredential":"c"})");
    Json start;
    REQUIRE(f.session->waitFor(core::msg::SessionStart, &start));
    CHECK(start["packet"]["appId"] == "tidal");
    CHECK(start["socketId"].get<std::uint32_t>() != 0);
    CHECK_FALSE(f.playback->received(core::msg::PlaybackPlay));

    // Session unblocks the socket, then play goes through.
    f.clientcomm->postMessage(core::msg::ClientcommBlockPacket, {{"socketId", start["socketId"]}, {"enable", false}});
    phone.send(R"({"command":"play","requestId":2})");
    Json play;
    REQUIRE(f.playback->waitFor(core::msg::PlaybackPlay, &play));
    CHECK(play["packet"]["requestId"] == 2);
}

TEST_CASE("unicast, multicast and session-requested close") {
    Fixture f;
    test::WsClient phone(kPort);
    REQUIRE(phone.waitConnected());
    phone.send(R"({"command":"startSession","appId":"tidal","appName":"TIDAL","sessionCredential":"c"})");
    Json start;
    REQUIRE(f.session->waitFor(core::msg::SessionStart, &start));
    const auto socketId = start["socketId"];

    // Blocked socket: multicast of a normal command is withheld, notifySessionError is not.
    f.clientcomm->postMessage(core::msg::SendPacket, {{"packet", {{"command", "notifyMediaChanged"}}}});
    f.clientcomm->postMessage(core::msg::SendPacket,
                              {{"packet", {{"command", "notifySessionError"}, {"errorCode", 1002}}}});
    CHECK(Json::parse(phone.receive())["command"] == "notifySessionError");

    f.clientcomm->postMessage(core::msg::ClientcommBlockPacket, {{"socketId", socketId}, {"enable", false}});
    f.clientcomm->postMessage(core::msg::SendPacket,
                              {{"socketId", socketId}, {"packet", {{"command", "notifySessionStarted"}, {"joined", false}}}});
    CHECK(Json::parse(phone.receive())["command"] == "notifySessionStarted");

    f.clientcomm->postMessage(core::msg::ClientcommCloseSocket, {{"socketId", socketId}});
    CHECK(phone.waitClosed());
    CHECK_FALSE(f.session->waitFor(core::msg::SessionLostConnection));  // session asked; no report
}

TEST_CASE("a phone disconnecting is reported to the session") {
    Fixture f;
    {
        test::WsClient phone(kPort);
        REQUIRE(phone.waitConnected());
    }
    CHECK(f.session->waitFor(core::msg::SessionLostConnection));
}
