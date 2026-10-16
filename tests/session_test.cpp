#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "core/MessageIds.hpp"
#include "core/SessionManager.hpp"

using namespace std::chrono_literals;
using namespace tcng::core;

namespace {

class Recorder : public Service {
public:
    explicit Recorder(std::string name) : Service(std::move(name)) {}
    // Waits until a message with id matching pred arrives; consumes it.
    bool waitFor(std::uint32_t id, Json* out = nullptr, std::function<bool(const Json&)> pred = {}) {
        std::unique_lock l(m);
        return cv.wait_for(l, 2s, [&] {
            for (auto it = got.begin(); it != got.end(); ++it)
                if (it->first == id && (!pred || pred(it->second))) {
                    if (out)
                        *out = it->second;
                    got.erase(it);
                    return true;
                }
            return false;
        });
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

struct Fixture {
    std::shared_ptr<ServiceManager> manager = std::make_shared<ServiceManager>();
    std::shared_ptr<SessionManager> session = std::make_shared<SessionManager>();
    std::shared_ptr<Recorder> clientcomm = std::make_shared<Recorder>(service::clientcomm);
    std::shared_ptr<Recorder> speaker = std::make_shared<Recorder>(service::speaker);
    std::shared_ptr<Recorder> playback = std::make_shared<Recorder>(service::playback);
    std::shared_ptr<Recorder> queue = std::make_shared<Recorder>(service::queue);

    Fixture() {
        for (auto s : std::vector<std::shared_ptr<Service>>{clientcomm, queue, playback, session, speaker})
            manager->registerService(s);
        manager->startAll();
    }
    ~Fixture() { manager->finishAll(); }

    Json start(std::uint32_t sock, const char* cred) {
        session->postMessage(msg::SessionStart,
                             {{"socketId", sock}, {"packet", {{"appId", "tidal"}, {"appName", "TIDAL"}, {"sessionCredential", cred}}}});
        return {};
    }
    static auto command(const char* c) {
        return [c](const Json& j) { return j["packet"]["command"] == c; };
    }
};

}  // namespace

TEST_CASE("startSession → requestResources → grant → notifySessionStarted, CASTING") {
    Fixture f;
    f.start(11, "cred");
    REQUIRE(f.speaker->waitFor(msg::SpeakerRequestResources));
    f.session->postMessage(msg::SessionGrantResources, Json());

    Json started;
    REQUIRE(f.clientcomm->waitFor(msg::SendPacket, &started, Fixture::command("notifySessionStarted")));
    CHECK(started["socketId"] == 11);
    CHECK(started["packet"]["joined"] == false);
    Json state;
    REQUIRE(f.playback->waitFor(msg::NotifySessionState, &state, [](const Json& j) { return j["state"] == 2; }));
    CHECK(state["appInfo"]["appId"] == "tidal");
    CHECK(f.session->getState() == SessionState::Casting);
}

TEST_CASE("same credential joins, different credential takes over") {
    Fixture f;
    f.start(11, "cred");
    f.session->postMessage(msg::SessionGrantResources, Json());
    REQUIRE(f.clientcomm->waitFor(msg::SendPacket, nullptr, Fixture::command("notifySessionStarted")));

    f.start(22, "cred");
    Json joined;
    REQUIRE(f.clientcomm->waitFor(msg::SendPacket, &joined, Fixture::command("notifySessionStarted")));
    CHECK(joined["socketId"] == 22);
    CHECK(joined["packet"]["joined"] == true);
    CHECK(f.queue->waitFor(msg::RequestStatusNotification));

    f.start(33, "other");
    CHECK(f.queue->waitFor(msg::QueuePurge));
    Json ended;
    REQUIRE(f.clientcomm->waitFor(msg::SendPacket, &ended, Fixture::command("notifySessionEnded")));
    CHECK(ended["packet"]["suspended"] == false);
    Json takeover;
    REQUIRE(f.clientcomm->waitFor(msg::SendPacket, &takeover, Fixture::command("notifySessionStarted")));
    CHECK(takeover["socketId"] == 33);
}

TEST_CASE("endSession with stopCasting releases resources and goes IDLE") {
    Fixture f;
    f.start(11, "cred");
    f.session->postMessage(msg::SessionGrantResources, Json());
    Json started;
    REQUIRE(f.clientcomm->waitFor(msg::SendPacket, &started, Fixture::command("notifySessionStarted")));
    const auto sid = started["packet"]["sessionId"];

    f.session->postMessage(msg::SessionEnd, {{"socketId", 11}, {"packet", {{"sessionId", sid}, {"stopCasting", true}}}});
    CHECK(f.speaker->waitFor(msg::SpeakerReleaseResources));
    CHECK(f.clientcomm->waitFor(msg::ClientcommCloseSocket));
    REQUIRE(f.playback->waitFor(msg::NotifySessionState, nullptr, [](const Json& j) { return j["state"] == 0; }));
}

TEST_CASE("errors are multicast notifySessionError and close the socket") {
    Fixture f;
    f.session->postMessage(msg::SessionResume, {{"socketId", 5}, {"packet", {{"sessionId", "x"}}}});
    Json err;
    REQUIRE(f.clientcomm->waitFor(msg::SendPacket, &err, Fixture::command("notifySessionError")));
    CHECK_FALSE(err.contains("socketId"));
    CHECK(err["packet"]["errorCode"] == 1101);
    CHECK(f.clientcomm->waitFor(msg::ClientcommCloseSocket));
}
