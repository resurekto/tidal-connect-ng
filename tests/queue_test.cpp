#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>

#include "core/MessageIds.hpp"
#include "queue/QueueManager.hpp"
#include "queue/QueueWindow.hpp"
#include "queue/QueueWindowLoader.hpp"

using namespace std::chrono_literals;
using namespace tcng;
using core::Json;
using queue::QueueItem;
using queue::QueueWindow;
using Report = QueueWindow::Report;

namespace {

QueueItem item(const std::string& id, bool active = false) {
    Json j{{"id", id}, {"media_id", "m" + id}, {"display_info", {{"title", "T" + id}}}};
    if (active)
        j["properties"] = {{"active", "true"}};
    return QueueItem(j, false);
}

std::vector<QueueItem> items(int n) {
    std::vector<QueueItem> v;
    for (int i = 0; i < n; ++i)
        v.push_back(item(std::to_string(i)));
    return v;
}

class Recorder : public core::Service {
public:
    explicit Recorder(std::string name) : Service(std::move(name)) {}
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

auto packetCommand(const char* c) {
    return [c](const Json& j) { return j["packet"]["command"] == c; };
}

}  // namespace

TEST_CASE("QueueItem parses cloud and local formats") {
    QueueItem cloud(Json{{"id", "i1"},
                         {"media_id", "123"},
                         {"type", "video"},
                         {"display_info",
                          {{"title", "Song"},
                           {"artists", {"A", "B"}},
                           {"album_title", "Album"},
                           {"duration_ms", 180000},
                           {"images", {{"high", {{"url", "u"}, {"width", 640}, {"height", 480}}}}}}},
                         {"properties", {{"active", "true"}}}},
                    false);
    CHECK(cloud.getItemId() == "i1");
    CHECK(cloud.getMediaType() == QueueItem::MediaType::Video);
    CHECK(cloud.getArtists().size() == 2);
    CHECK(cloud.getImages().at("high").height == 480);
    CHECK(cloud.isActive());

    QueueItem local(Json{{"itemId", "i2"},
                         {"mediaId", "456"},
                         {"metadata", {{"title", "X"}, {"duration", 1000}}},
                         {"policy", {{"canNext", true}, {"canPrevious", false}}}},
                    true);
    CHECK(local.getPolicy().canNext);
    CHECK_FALSE(local.getPolicy().canPrevious);

    auto info = cloud.makeMediaInfo();
    CHECK(info["metadata"]["title"] == "Song");
    CHECK(info["metadata"]["duration"] == 180000);
    CHECK(info["mediaId"] == "123");
}

TEST_CASE("QueueWindow navigation, prefetch and edges") {
    QueueWindow w("q", queue::RepeatMode::Off, false, 20, 20, item("3"));
    Report r;
    CHECK_FALSE(w.next(r));
    CHECK(r == Report::NotLoaded);

    w.storeQueueItems(items(10), true, false);  // current "3": 3 before, 6 after
    auto n = w.next(r);
    REQUIRE(n);
    CHECK(n->getItemId() == "4");
    CHECK(r == Report::Ok);  // 5 after: not yet below the threshold of 5
    n = w.next(r);
    CHECK(n->getItemId() == "5");
    CHECK(r == Report::OkPrefetch);  // 4 after, no end: prefetch

    auto peek = w.getNext(r);
    REQUIRE(peek);
    CHECK(peek->getItemId() == "6");

    for (int i = 0; i < 5; ++i)
        w.previous(r);
    CHECK_FALSE(w.previous(r));
    CHECK(r == Report::Begin);
}

TEST_CASE("an active (one-shot) first item is dropped without skipping the next one") {
    std::vector<QueueItem> v{item("0", true), item("1"), item("2")};
    QueueWindow w("q", queue::RepeatMode::Off, false, 20, 20, item("0", true));
    w.storeQueueItems(v, true, true);
    Report r;
    auto n = w.next(r);
    REQUIRE(n);
    CHECK(n->getItemId() == "1");
    CHECK(w.getItems().size() == 2);
}

TEST_CASE("loader builds the queue-server requests") {
    queue::QueueWindowLoader loader({});
    queue::QueueWindowLoader::ServerInfo si;
    si.serverUrl = "https://q.example/v1/queues";
    si.oauthAccessToken = "tok";
    si.httpHeaderFields = {"X-Extra: 1"};
    loader.setServerInfo(si);
    QueueWindow w("Q1", queue::RepeatMode::All, true, 20, 10, item("7"));

    auto get = loader.buildGetWindowRequest(w);
    CHECK(get.url == "https://q.example/v1/queues/Q1/window");
    CHECK(get.query.at("itemid") == "7");
    CHECK(get.query.at("aftersize") == "10");
    CHECK(get.headers == std::vector<std::string>{"Authorization: Bearer tok", "X-Extra: 1"});

    auto put = loader.buildPutRepeatModeRequest(w, queue::RepeatMode::One);
    CHECK(put.url == "https://q.example/v1/queues/Q1/repeatmode");
    CHECK(Json::parse(put.body) == Json{{"repeat_mode", "one"}});

    tcng::util::HttpResponse resp;
    const std::string body = R"({"items":[{"id":"a"},{"id":"b"}],"includes_head":true,"includes_tail":false})";
    resp.body.assign(body.begin(), body.end());
    std::vector<QueueItem> parsed;
    bool head = false, tail = true;
    REQUIRE(queue::QueueWindowLoader::parseGetWindowResponse(resp, parsed, head, tail));
    CHECK(parsed.size() == 2);
    CHECK(head);
    CHECK_FALSE(tail);
}

TEST_CASE("local queue: load, requestQueueItems, respond, next") {
    auto manager = std::make_shared<core::ServiceManager>();
    auto q = std::make_shared<queue::QueueManager>();
    auto playback = std::make_shared<Recorder>(core::service::playback);
    auto clientcomm = std::make_shared<Recorder>(core::service::clientcomm);
    auto speaker = std::make_shared<Recorder>(core::service::speaker);
    for (auto s : std::vector<std::shared_ptr<core::Service>>{clientcomm, q, playback, speaker})
        manager->registerService(s);
    manager->startAll();

    const Json current{{"itemId", "2"}, {"mediaId", "m2"}, {"metadata", {{"title", "two"}}}};
    q->postMessage(core::msg::QueueLoadLocalQueue,
                   {{"socketId", 77},
                    {"packet",
                     {{"command", "loadLocalQueue"},
                      {"requestId", 1},
                      {"queueInfo", {{"queueId", "Q"}, {"repeatMode", "OFF"}, {"shuffled", false}}},
                      {"currentMediaInfo", current},
                      {"position", 0},
                      {"autoplay", true}}}});

    Json setMedia;
    REQUIRE(playback->waitFor(core::msg::PlaybackSetMedia, &setMedia));
    CHECK(setMedia["mediaInfo"]["itemId"] == "2");
    CHECK(playback->waitFor(core::msg::PlaybackPlay));
    CHECK(clientcomm->waitFor(core::msg::SendPacket, nullptr, packetCommand("notifyQueueChanged")));

    Json req;
    REQUIRE(clientcomm->waitFor(core::msg::SendPacket, &req, packetCommand("requestQueueItems")));
    CHECK(req["socketId"] == 77);

    Json mediaInfos = Json::array();
    for (int i = 0; i < 5; ++i)
        mediaInfos.push_back({{"itemId", std::to_string(i)}, {"mediaId", "m" + std::to_string(i)}, {"metadata", {{"title", "t"}}}});
    q->postMessage(core::msg::QueueRespondQueueItems,
                   {{"socketId", 77},
                    {"packet", {{"requestId", 2},
                                {"queueRequestId", req["packet"]["queueRequestId"]},
                                {"mediaInfos", mediaInfos},
                                {"hasBegin", true},
                                {"hasEnd", true}}}});

    Json result;
    REQUIRE(clientcomm->waitFor(core::msg::SendPacket, &result, [](const Json& j) {
        return j["packet"]["command"] == "notifyRequestResult" && j["packet"]["requestId"] == 1;
    }));
    CHECK(result["packet"]["resultCode"] == 0);

    q->postMessage(core::msg::QueueNext, {{"socketId", 77}, {"packet", {{"requestId", 3}}}});
    REQUIRE(playback->waitFor(core::msg::PlaybackSetMedia, &setMedia));
    CHECK(setMedia["mediaInfo"]["itemId"] == "3");
    CHECK(q->getState() == queue::QueueManager::State::Loaded);
    manager->finishAll();
}
