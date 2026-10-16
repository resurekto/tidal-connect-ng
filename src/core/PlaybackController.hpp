#pragma once
// The "playback" service: picks the engine for the casting
// app, drives it from queue/app commands, and reports player status, media and errors to
// the app and the speaker controller.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "core/MessageCore.hpp"
#include "media/IMediaPlayer.hpp"
#include "media/PlaybackEngineGeneric.hpp"

namespace tcng::core {

class PlaybackController : public Service {
public:
    enum class State : int { NoEngine = 0, Idle = 1, Preparing = 2, Started = 3, Completed = 4, Stopped = 5 };
    enum class PlayState : int { None = 0, Play = 1, Pause = 2 };

    explicit PlaybackController(std::shared_ptr<media::IMediaPlayer> player);
    ~PlaybackController() override;

    // appId → engine; unknown apps use the default generic engine.
    void registerPlaybackEngine(const std::string& appId, const std::shared_ptr<media::PlaybackEngineGeneric>& engine);

    void start() override;
    void finish() override;

    State getState() const { return state_.load(); }

protected:
    void handleMessage(std::uint32_t id, const Json& payload) override;

private:
    class EngineDelegate;
    using Engine = media::PlaybackEngineGeneric;

    // Engine callbacks (engine / player threads)
    void onPrepared(std::uint32_t seq, const media::AudioItem& item);
    void onPreparedNext(std::uint32_t seq, const media::AudioItem& item);
    void onStatusUpdated(const media::PlayStatus& status);
    void onProgressUpdated(std::uint64_t positionMs, std::uint64_t durationMs);
    void onAudioFormatUpdated(const media::AudioFormat& format);
    void onPlaybackCompleted(std::uint32_t seq, bool hasNext);
    void onPrepareError(std::uint32_t seq, Engine::RecoveryAction action, const util::HttpResponse& response);
    void onPrepareNextError(std::uint32_t seq, Engine::RecoveryAction action, const util::HttpResponse& response);
    void onPlaybackError(std::uint32_t seq, const media::PlaybackError& error);
    void onRequestNextMedia(std::uint32_t seq);

    // Handlers (playback thread)
    void procPlay(const Json& j);
    void procPause(const Json& j);
    void procStop(const Json& j);
    void procSeek(const Json& j);
    void procReset();
    void procSetServerInfo(const Json& j);
    void procSetMedia(const Json& j);
    void procSetNextMedia(const Json& j);
    void procInterruptSpeaker(const Json& j);
    void procNotifyPrepared(const Json& j);
    void procNotifyPreparedNext(const Json& j);
    void procNotifyMpCompleted(const Json& j);
    void procNotifyMpRequestNextMedia(const Json& j);
    void procNotifySessionState(const Json& j);
    void procRequestStatusNotification();

    void updateState(State state);
    void updatePlayState(PlayState state);
    std::shared_ptr<Engine> lookupEngine(const std::string& appId) const;
    bool hasPlayable(const Json& mediaInfo) const;
    void prepare(const Json& mediaInfo, bool next);
    void resetMedia();

    // Outbound
    void appNotifyPlayerStatusChanged();
    void appNotifyMediaChanged(const Json& mediaInfo);
    void appNotifyPlaybackError(const media::PlaybackError& error);
    void appNotifyContentServerError(const util::HttpResponse& response);
    void appNotifyRequestResult(const Json& req, int code);
    void queueSend(std::uint32_t id, const Json& payload);

    std::shared_ptr<media::IMediaPlayer> player_;
    std::shared_ptr<EngineDelegate> delegate_;
    std::shared_ptr<Engine> defaultEngine_;
    std::map<std::string, std::shared_ptr<Engine>> engines_;
    std::shared_ptr<Engine> activeEngine_;  // playback thread only

    std::atomic<State> state_{State::NoEngine};
    std::atomic<PlayState> playState_{PlayState::None};
    std::atomic<std::uint32_t> mediaSeqNo_{0};
    std::atomic<media::playstate_t> playerState_{media::playstate_t::Idle};
    std::atomic<std::uint64_t> progress_{0};
    std::atomic<std::uint64_t> duration_{0};
    std::mutex notifyMutex_;  // serialises appNotifyPlayerStatusChanged

    // Playback thread only.
    Json currentMedia_;
    Json nextMedia_;
    std::uint64_t pendingSeek_ = 0;
    std::optional<media::ServerInfo> serverInfo_;
};

}  // namespace tcng::core
