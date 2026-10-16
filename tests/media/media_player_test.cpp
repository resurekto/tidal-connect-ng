#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include <thread>

#include "FlacPackage.hpp"
#include "test_util/MiniHttpServer.hpp"
#include "media/MediaPlayer.hpp"
#include "util/HttpClient.hpp"

using namespace tcng::media;
using namespace std::chrono_literals;

namespace {

class FakeSink : public IPcmSink {
public:
    std::atomic<bool> opened{false};
    std::atomic<int> opens{0};
    int open(int, SampleType, bool) override {
        opened = true;
        ++opens;
        return 0;
    }
    int close() override {
        opened = false;
        return 0;
    }
    bool isOpen() const override { return opened; }
    int start() override { return 0; }
    int stop() override { return 0; }
    int pause() override { return 0; }
    long writeFrames(const void*, unsigned long) override { return 0; }
    long getFramesAvailable() override { return 0; }
    void setDelegate(Delegate* d) override { delegate = d; }
    Delegate* delegate = nullptr;
};

class Listener : public IMediaPlayer::Delegate {
public:
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> events;

    void onStatusUpdated(const PlayStatus& s, const AudioItem&) override { add(std::string("status:") + toString(s.state)); }
    void onProgressUpdated(std::uint64_t, std::uint64_t) override { add("progress"); }
    void onAudioFormatUpdated(const AudioFormat&) override { add("format"); }
    void onPlaybackCompleted(bool hasNext) override { add(hasNext ? "completed:1" : "completed:0"); }
    void onPlaybackError(const PlaybackError& e) override { add("error:" + std::to_string(e.trackStatus)); }
    void onRequestNextMedia() override { add("requestNext"); }

    bool waitFor(const std::string& e, std::chrono::milliseconds timeout = 10s) {
        std::unique_lock l(m);
        return cv.wait_for(l, timeout, [&] { return std::find(events.begin(), events.end(), e) != events.end(); });
    }

private:
    void add(const std::string& e) {
        std::lock_guard l(m);
        events.push_back(e);
        cv.notify_all();
    }
};

}  // namespace

TEST_CASE("MediaPlayer: prepare pauses, play buffers, stop returns to idle") {
    auto sink = std::make_shared<FakeSink>();
    MediaPlayer player(sink);
    Listener listener;
    player.connect(&listener);
    CHECK(sink->delegate != nullptr);

    AudioItem item;
    item.url = "http://127.0.0.1:1/never";  // nothing listens: fetches fail fast
    player.prepare(item);
    REQUIRE(listener.waitFor("status:PAUSED"));
    PlayStatus st;
    player.getStatus(st);
    CHECK(st.state == playstate_t::Paused);

    player.play();
    REQUIRE(listener.waitFor("status:BUFFERING"));
    player.stop();
    player.getStatus(st);
    CHECK(st.state == playstate_t::Idle);
    CHECK(listener.waitFor("status:IDLE"));
    player.disconnect();
}

TEST_CASE("MediaPlayer: an unreachable stream ends in a max-connections playback error") {
    tcng::util::HttpClient::globalInit("");
    auto sink = std::make_shared<FakeSink>();
    MediaPlayer player(sink);
    Listener listener;
    player.connect(&listener);
    AudioItem item;
    item.url = "http://127.0.0.1:1/never";
    player.prepare(item);
    player.play();
    CHECK(listener.waitFor("error:9"));
    player.stop();
    player.disconnect();
}

TEST_CASE("MediaPlayer: with no audio data the sink callback outputs silence and counts underruns") {
    auto sink = std::make_shared<FakeSink>();
    MediaPlayer player(sink);
    std::vector<std::uint8_t> out(256 * 4, 0xff);
    sink->delegate->onFramesRequested(out.data(), 256);
    CHECK(std::all_of(out.begin(), out.end(), [](std::uint8_t b) { return b == 0; }));
    CHECK(player.underrunCount() == 1);
}

namespace {

// Status and progress values in arrival order.
class TimelineListener : public IMediaPlayer::Delegate {
public:
    std::mutex m;
    std::vector<std::string> states;
    std::vector<std::uint64_t> progress;

    void onStatusUpdated(const PlayStatus& s, const AudioItem&) override {
        std::lock_guard l(m);
        states.push_back(toString(s.state));
    }
    void onProgressUpdated(std::uint64_t pos, std::uint64_t) override {
        std::lock_guard l(m);
        progress.push_back(pos);
    }
    void onAudioFormatUpdated(const AudioFormat&) override {}
    void onPlaybackCompleted(bool) override {}
    void onPlaybackError(const PlaybackError&) override {}
    void onRequestNextMedia() override {}

    template <typename Pred>
    bool waitFor(Pred pred, std::chrono::milliseconds timeout = 10s) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < end) {
            {
                std::lock_guard l(m);
                if (pred())
                    return true;
            }
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }
};

}  // namespace

TEST_CASE("MediaPlayer: a seek during playback of a segmented stream reports the target and stays PLAYING") {
    tcng::util::HttpClient::globalInit("");
    const unsigned rate = 44100;
    const auto flac = tcng::test::encode(16, rate, rate * 8);
    const auto pkg = tcng::test::package(flac, rate, 10);  // ~0.93 s segments
    tcng::test::MiniHttpServer server;
    auto url = [&](std::size_t i) {
        const std::string path = "/" + std::to_string(i) + ".mp4";
        server.serve(path, pkg.bodies[i]);
        return server.url(path);
    };
    AudioItem item;
    item.mediaformat = media_format::kFlac;
    item.audioCodec = audio_codec::kFlac;
    item.initUrl = item.url = url(0);
    for (std::size_t i = 0; i < pkg.descriptor.segments.size(); ++i)
        item.segments.push_back({url(i + 1), pkg.descriptor.segments[i].durationSeconds});
    item.durationSeconds = 8.0;

    TimelineListener listener;  // outlives the player, which calls it from its thread
    auto sink = std::make_shared<FakeSink>();
    MediaPlayer player(sink);
    player.connect(&listener);
    // The audio thread: pull 10 ms of PCM every 10 ms while the output is open.
    std::atomic<bool> running{true};
    struct Joiner {
        std::atomic<bool>& running;
        std::thread t;
        ~Joiner() {
            running = false;
            if (t.joinable())
                t.join();
        }
    } audio{running, std::thread([&] {
        std::vector<std::uint8_t> buf(441 * 4);
        while (running) {
            if (sink->opened && sink->delegate)
                sink->delegate->onFramesRequested(buf.data(), 441);
            std::this_thread::sleep_for(10ms);
        }
    })};

    player.prepare(item);
    player.play();
    REQUIRE(listener.waitFor([&] { return !listener.progress.empty() && listener.progress.back() >= 1000; }));
    std::size_t statesBefore = 0;
    {
        std::lock_guard l(listener.m);
        statesBefore = listener.states.size();
        listener.progress.clear();
        CHECK(listener.states.back() == "PLAYING");
    }

    server.setDelay(300ms);  // the reload takes a while, like a hi-res segment
    player.seekTo(6000);     // another segment: restart + reload
    REQUIRE(listener.waitFor([&] { return !listener.progress.empty() && listener.progress.back() >= 6500; }));
    {
        std::lock_guard l(listener.m);
        CHECK(listener.progress.front() == 6000);  // the target, before any audio
        for (const auto p : listener.progress)
            CHECK(p >= 6000);
        const std::vector<std::string> after(listener.states.begin() + static_cast<long>(statesBefore), listener.states.end());
        CHECK(std::find(after.begin(), after.end(), "BUFFERING") == after.end());
    }
    player.stop();
}
