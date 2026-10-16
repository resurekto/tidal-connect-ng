#pragma once
// Test doubles for the media engine tests.

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "media/IMediaPlayer.hpp"
#include "media/PlaybackEngineGeneric.hpp"

namespace tcng::test {

using namespace tcng::media;

class FakePlayer : public IMediaPlayer {
public:
    std::vector<std::string> calls;
    std::vector<AudioItem> prepared;
    Delegate* delegate = nullptr;

    int init() override { return record("init"); }
    int deinit() override { return record("deinit"); }
    int connect(Delegate* d) override {
        delegate = d;
        return record("connect");
    }
    int disconnect() override {
        delegate = nullptr;
        return record("disconnect");
    }
    int prepare(const AudioItem& item) override {
        std::lock_guard l(m);
        prepared.push_back(item);
        calls.push_back("prepare");
        return kSuccess;
    }
    int prepareNext(const AudioItem& item) override {
        std::lock_guard l(m);
        prepared.push_back(item);
        calls.push_back("prepareNext");
        return kSuccess;
    }
    int play() override { return record("play"); }
    int pause() override { return record("pause"); }
    int stop() override { return record("stop"); }
    int seekTo(std::uint64_t ms) override { return record("seekTo:" + std::to_string(ms)); }
    int getStatus(PlayStatus& s) override {
        s.state = playstate_t::Idle;
        return kSuccess;
    }
    int interrupt(bool) override { return record("interrupt"); }

    std::mutex m;

private:
    int record(const std::string& c) {
        std::lock_guard l(m);
        calls.push_back(c);
        return kSuccess;
    }
};

class RecordingDelegate : public PlaybackEngineGeneric::Delegate {
public:
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> events;
    util::HttpResponse lastResponse;
    PlaybackEngineGeneric::RecoveryAction lastAction = PlaybackEngineGeneric::RecoveryAction::SkipTrack;

    void onPrepared(std::uint32_t seq, const AudioItem& item) override { add("prepared:" + std::to_string(seq) + ":" + item.url); }
    void onStatusUpdated(const PlayStatus& s, const AudioItem&) override { add("status:" + std::string(toString(s.state))); }
    void onProgressUpdated(std::uint64_t p, std::uint64_t d) override { add("progress:" + std::to_string(p) + "/" + std::to_string(d)); }
    void onAudioFormatUpdated(const AudioFormat& f) override { add("format:" + std::to_string(f.audioSamplingRate)); }
    void onPlaybackCompleted(std::uint32_t seq, bool hasNext) override {
        add("completed:" + std::to_string(seq) + ":" + (hasNext ? "1" : "0"));
    }
    void onPrepareError(std::uint32_t seq, PlaybackEngineGeneric::RecoveryAction a, const util::HttpResponse& r) override {
        {
            std::lock_guard l(m);
            lastAction = a;
            lastResponse = r;
        }
        add("prepareError:" + std::to_string(seq));
    }
    void onPrepareNextError(std::uint32_t seq, PlaybackEngineGeneric::RecoveryAction a, const util::HttpResponse& r) override {
        {
            std::lock_guard l(m);
            lastAction = a;
            lastResponse = r;
        }
        add("prepareNextError:" + std::to_string(seq));
    }
    void onPlaybackError(std::uint32_t seq, const PlaybackError& e) override {
        add("playbackError:" + std::to_string(seq) + ":" + std::to_string(e.trackStatus));
    }
    void onRequestNextMedia(std::uint32_t seq) override { add("requestNext:" + std::to_string(seq)); }
    void onPreparedNext(std::uint32_t seq, const AudioItem& item) override {
        add("preparedNext:" + std::to_string(seq) + ":" + item.url);
    }

    template <typename Pred>
    bool waitFor(Pred pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        std::unique_lock l(m);
        return cv.wait_for(l, timeout, [&] { return pred(events); });
    }

    bool contains(const std::string& e) {
        std::lock_guard l(m);
        for (const auto& x : events)
            if (x == e)
                return true;
        return false;
    }

private:
    void add(const std::string& e) {
        std::lock_guard l(m);
        events.push_back(e);
        cv.notify_all();
    }
};

}  // namespace tcng::test
