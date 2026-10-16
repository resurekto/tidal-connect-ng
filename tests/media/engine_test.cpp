#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

#include "FakePlayer.hpp"
#include "media/PlaybackEngineTidal.hpp"
#include "util/HttpClient.hpp"

using namespace tcng::media;
using namespace tcng::test;
using RA = PlaybackEngineGeneric::RecoveryAction;

namespace {

tcng::util::HttpResponse resp(int status, const std::string& body) {
    tcng::util::HttpResponse r;
    r.status = status;
    r.body.assign(body.begin(), body.end());
    return r;
}

bool hasEvent(const std::vector<std::string>& ev, const std::string& prefix) {
    return std::any_of(ev.begin(), ev.end(), [&](const std::string& e) { return e.rfind(prefix, 0) == 0; });
}

}  // namespace

TEST_CASE("checkStatusCode -> RecoveryAction") {
    bool clear = true;
    CHECK(PlaybackEngineTidal::classifyStatus(resp(400, R"({"subStatus":4005})"), &clear) == RA::SkipTrack);
    CHECK_FALSE(clear);
    CHECK(PlaybackEngineTidal::classifyStatus(resp(404, R"({"sub_status":4007})"), &clear) == RA::SkipTrack);
    CHECK(PlaybackEngineTidal::classifyStatus(resp(500, R"({"subStatus":4005})"), &clear) == RA::Default);
    CHECK(PlaybackEngineTidal::classifyStatus(resp(401, R"({"subStatus":11002})"), &clear) == RA::Default);
    CHECK(clear);
    CHECK(PlaybackEngineTidal::classifyStatus(resp(401, R"({"subStatus":11004})"), &clear) == RA::Default);
    CHECK_FALSE(clear);
    CHECK(PlaybackEngineTidal::classifyStatus(resp(401, R"({"subStatus":"4005"})"), &clear) == RA::Default);
    CHECK(PlaybackEngineTidal::classifyStatus(resp(99, ""), &clear) == RA::Default);
}

TEST_CASE("generic engine: direct URL prepare, stale prepareNext, stop") {
    auto player = std::make_shared<FakePlayer>();
    auto delegate = std::make_shared<RecordingDelegate>();
    auto engine = std::make_shared<PlaybackEngineGeneric>();
    engine->connect(player, delegate);
    CHECK(player->delegate == engine.get());

    engine->prepare(7, "http://a/1");
    CHECK(player->prepared.back().url == "http://a/1");
    CHECK(delegate->contains("prepared:7:http://a/1"));
    CHECK(engine->prepareNext(6, "http://a/2") == 1);
    CHECK(engine->prepareNext(7, "http://a/2") == kSuccess);
    CHECK(delegate->contains("preparedNext:7:http://a/2"));

    // player callbacks carry the engine's mediaSeqNo
    engine->onPlaybackCompleted(true);
    engine->onRequestNextMedia();
    CHECK(delegate->contains("completed:7:1"));
    CHECK(delegate->contains("requestNext:7"));

    engine->seekTo(1500);
    engine->stop();
    CHECK(engine->currentSeq() == 0);
    CHECK(delegate->contains("status:IDLE"));
    CHECK(delegate->contains("progress:0/0"));
    CHECK(std::find(player->calls.begin(), player->calls.end(), "seekTo:1500") != player->calls.end());
    engine->disconnect();
    CHECK(player->delegate == nullptr);
}

TEST_CASE("generic engine: ServerInfo URL fetch request") {
    ServerInfo si;
    si.serverUrl = "https://media.example.com/resolve";
    si.httpHeaderFields = {"X-A: 1"};
    const tcng::util::HttpRequest req = PlaybackEngineGeneric::buildUrlFetchRequest(si, "m1");
    CHECK(req.url == "https://media.example.com/resolve");
    CHECK(req.host == "media.example.com");
    CHECK(req.query.at("media_id") == "m1");
    CHECK(req.headers == std::vector<std::string>{"X-A: 1"});
}

TEST_CASE("tidal engine: failed token switch reports a prepare error on the pool thread") {
    tcng::util::HttpClient::globalInit("");
    auto player = std::make_shared<FakePlayer>();
    auto delegate = std::make_shared<RecordingDelegate>();
    auto engine = std::make_shared<PlaybackEngineTidal>("CID");
    engine->connect(player, delegate);

    ServerInfo si;
    si.serverUrl = "http://127.0.0.1:1";
    si.headerAuth = "Bearer APP";
    si.oauth.serverUrl = "http://127.0.0.1:1/token";  // connection refused
    engine->prepare(3, si, "123", 0);
    REQUIRE(delegate->waitFor([](const auto& ev) { return hasEvent(ev, "prepareError:3"); }));
    CHECK(delegate->lastAction == RA::Default);
    CHECK(delegate->lastResponse.status == 99);
    CHECK(player->prepared.empty());

    // prepareNext with a stale seq is rejected synchronously
    CHECK(engine->prepareNext(2, si, "124", 0) == 1);

    // no access token at all -> error without any request
    ServerInfo empty;
    engine->prepare(4, empty, "1", 0);
    REQUIRE(delegate->waitFor([](const auto& ev) { return hasEvent(ev, "prepareError:4"); }));
    engine->disconnect();
}

TEST_CASE("tidal engine: a superseded fetch is discarded") {
    tcng::util::HttpClient::globalInit("");
    auto player = std::make_shared<FakePlayer>();
    auto delegate = std::make_shared<RecordingDelegate>();
    auto engine = std::make_shared<PlaybackEngineTidal>("CID");
    engine->connect(player, delegate);
    ServerInfo si;
    si.headerAuth = "Bearer APP";
    si.oauth.serverUrl = "http://127.0.0.1:1/token";
    engine->prepare(10, si, "a", 0);
    engine->prepare(11, si, "b", 0);
    REQUIRE(delegate->waitFor([](const auto& ev) { return hasEvent(ev, "prepareError:11"); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(delegate->contains("prepareError:10"));
    {
        std::lock_guard l(delegate->m);
        CHECK(std::count(delegate->events.begin(), delegate->events.end(), "prepareError:11") == 1);
    }
    engine->stop();
    engine->disconnect();
}
