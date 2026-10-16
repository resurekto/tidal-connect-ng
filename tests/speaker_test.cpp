#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "core/MessageCore.hpp"
#include "core/MessageIds.hpp"
#include "net/Loop.hpp"
#include "spkcon/SpeakerCommunicator.hpp"
#include "spkcon/SpkconPacket.hpp"
#include "test_util/Unique.hpp"
#include "test_util/UnixClient.hpp"

using namespace std::chrono_literals;
using namespace tcng;
using core::Json;

namespace {

// Records every message it receives.
class Recorder : public core::Service {
public:
    explicit Recorder(std::string name) : Service(std::move(name)) {}

    bool waitFor(std::uint32_t id, Json* out = nullptr) {
        std::unique_lock l(m);
        return cv.wait_for(l, 2s, [&] {
            for (auto& [mid, j] : got)
                if (mid == id) {
                    if (out)
                        *out = j;
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

Json readFrame(test::UnixClient& client) {
    std::string payload;
    bool done = false;
    spkcon::FrameDecoder d([&](std::string p) { payload = std::move(p); done = true; });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!done && std::chrono::steady_clock::now() < deadline)
        d.feed(client.readExactly(1, 100ms));
    return done ? Json::parse(payload) : Json();
}

void writeJson(test::UnixClient& client, const Json& j) {
    client.write(*spkcon::encodeFrame(j.dump()));
}

// Records what the speaker service applies; current() returns `device`.
class FakeVolume : public media::IVolumeControl {
public:
    std::mutex m;
    std::vector<media::VolumeState> applied;
    std::optional<media::VolumeState> device;

    const char* name() const override { return "fake"; }
    bool set(const media::VolumeState& s) override {
        std::lock_guard l(m);
        applied.push_back(s);
        return true;
    }
    std::optional<media::VolumeState> current() override {
        std::lock_guard l(m);
        return device;
    }
    std::size_t count() {
        std::lock_guard l(m);
        return applied.size();
    }
};

struct Fixture {
    std::string path = test::uniqueSocketPath("tcng-spk");
    std::shared_ptr<media::IVolumeControl> volume;
    net::Loop loop;
    std::shared_ptr<core::ServiceManager> manager = std::make_shared<core::ServiceManager>();
    std::shared_ptr<Recorder> session = std::make_shared<Recorder>(core::service::session);
    std::shared_ptr<Recorder> playback = std::make_shared<Recorder>(core::service::playback);
    std::shared_ptr<Recorder> clientcomm = std::make_shared<Recorder>(core::service::clientcomm);
    std::shared_ptr<spkcon::SpeakerCommunicator> speaker;

    explicit Fixture(std::shared_ptr<media::IVolumeControl> v = nullptr)
        : volume(std::move(v)),
          speaker(std::make_shared<spkcon::SpeakerCommunicator>(loop, spkcon::SpeakerCommunicator::Config{path, volume})) {
        loop.start();
        manager->registerService(clientcomm);
        manager->registerService(playback);
        manager->registerService(session);
        manager->registerService(speaker);
        manager->start(core::service::speaker);
        manager->startAll();
    }
    ~Fixture() {
        manager->finishAll();
        loop.stop();
    }
};

}  // namespace

TEST_CASE("requestResources is auto-granted when no controller is connected") {
    Fixture f;
    f.speaker->postMessage(core::msg::SpeakerRequestResources, {{"command", "requestResources"}});
    CHECK(f.session->waitFor(core::msg::SessionGrantResources));
}

TEST_CASE("controller setVolumeMute is validated and answered") {
    Fixture f;
    test::UnixClient client(f.path);
    REQUIRE(client.connected());
    std::this_thread::sleep_for(100ms);

    writeJson(client, {{"command", "setVolumeMute"}, {"requestId", 7}, {"volume", {{"level", 0.5}, {"mute", false}}}});
    auto reply = readFrame(client);
    CHECK(reply == Json{{"command", "notifyRequestResult"}, {"requestId", 7}, {"resultCode", 0}});

    writeJson(client, {{"command", "setVolumeMute"}, {"requestId", 8}, {"volume", {{"level", 1.5}, {"mute", false}}}});
    reply = readFrame(client);
    CHECK(reply["resultCode"] == 1001);
}

TEST_CASE("controller commands are routed with socketId 0, and requestResources goes to the controller") {
    Fixture f;
    test::UnixClient client(f.path);
    REQUIRE(client.connected());
    std::this_thread::sleep_for(100ms);

    writeJson(client, {{"command", "play"}, {"requestId", 3}});
    Json got;
    REQUIRE(f.playback->waitFor(core::msg::PlaybackPlay, &got));
    CHECK(got["socketId"] == 0);
    CHECK(got["packet"]["requestId"] == 3);

    f.speaker->postMessage(core::msg::SpeakerRequestResources, {{"command", "requestResources"}});
    CHECK(readFrame(client) == Json{{"command", "requestResources"}});
}

TEST_CASE("app setVolume is echoed to the controller and answered to the app") {
    Fixture f;
    test::UnixClient client(f.path);
    REQUIRE(client.connected());
    std::this_thread::sleep_for(100ms);

    f.speaker->postMessage(core::msg::SpeakerSetVolume, {{"socketId", 42}, {"packet", {{"requestId", 5}, {"level", 0.3}}}});
    auto echo = readFrame(client);
    CHECK(echo["command"] == "setVolumeMute");
    CHECK(echo["volume"]["level"] == 0.3);
    Json result;
    REQUIRE(f.clientcomm->waitFor(core::msg::SendPacket, &result));
    CHECK(result["socketId"] == 42);
    CHECK(result["packet"]["command"] == "notifyRequestResult");
}

TEST_CASE("volume control: the app's volume is applied, a controller's is not; the device level is reported") {
    auto fake = std::make_shared<FakeVolume>();
    fake->device = media::VolumeState{0.7, false};
    Fixture f(fake);
    std::this_thread::sleep_for(50ms);
    CHECK(fake->count() == 0);  // a readable device is read at start, not overwritten

    // Casting: the level reported to the app is the device's.
    fake->device = media::VolumeState{0.65, false};  // changed outside the app meanwhile
    f.speaker->postMessage(core::msg::NotifySessionState, {{"state", 2}});
    f.speaker->postMessage(core::msg::SpeakerSetMute, {{"socketId", 7}, {"packet", {{"requestId", 1}, {"mute", false}}}});
    Json status;
    REQUIRE(f.clientcomm->waitFor(core::msg::SendPacket, &status));
    CHECK(status["packet"]["command"] == "notifyDeviceStatusChanged");
    CHECK(status["packet"]["volume"]["level"] == 0.65);

    f.speaker->postMessage(core::msg::SpeakerSetVolume, {{"socketId", 7}, {"packet", {{"requestId", 2}, {"level", 0.3}}}});
    f.speaker->postMessage(core::msg::SpeakerSetMute, {{"socketId", 7}, {"packet", {{"requestId", 3}, {"mute", true}}}});
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (fake->count() < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    {
        std::lock_guard l(fake->m);
        REQUIRE(fake->applied.size() == 3);  // setMute(false), setVolume, setMute(true)
        CHECK(fake->applied[1].level == 0.3);
        CHECK_FALSE(fake->applied[1].mute);
        CHECK(fake->applied[2].level == 0.3);
        CHECK(fake->applied[2].mute);
    }

    // A controller reports a level it has applied itself: not applied again.
    test::UnixClient client(f.path);
    REQUIRE(client.connected());
    std::this_thread::sleep_for(100ms);
    writeJson(client, {{"command", "setVolumeMute"}, {"requestId", 9}, {"volume", {{"level", 0.5}, {"mute", false}}}});
    CHECK(readFrame(client)["command"] == "notifyRequestResult");
    CHECK(fake->count() == 3);
}

TEST_CASE("volume control: an unreadable control (software) starts at the level the app is shown") {
    auto fake = std::make_shared<FakeVolume>();  // current() = nullopt
    Fixture f(fake);
    std::this_thread::sleep_for(50ms);
    std::lock_guard l(fake->m);
    REQUIRE(fake->applied.size() == 1);
    CHECK(fake->applied[0].level == 0.2);  // initial level
}
