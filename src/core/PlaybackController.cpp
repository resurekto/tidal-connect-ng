#include "core/PlaybackController.hpp"

#include "util/JsonUtil.hpp"
#include "core/MessageIds.hpp"
#include "util/Log.hpp"

namespace tcng::core {

namespace {

constexpr int kResultSuccess = 0;
constexpr int kResultIllegalState = 1002;
constexpr int kErrorContentServer = 1012;

const char* stateName(PlaybackController::State s) {
    switch (s) {
    case PlaybackController::State::NoEngine: return "NO_ENGINE";
    case PlaybackController::State::Idle: return "IDLE";
    case PlaybackController::State::Preparing: return "PREPARING";
    case PlaybackController::State::Started: return "STARTED";
    case PlaybackController::State::Completed: return "COMPLETED";
    case PlaybackController::State::Stopped: return "STOPPED";
    }
    return "UNKNOWN";
}

int convertTrackStatus(int status) {
    switch (status) {
    case 8: return 1012;
    case 9: return 1011;
    case 0: case 2: case 3: case 4: case 5: case 6: case 7: case 10: return 1301;
    default:
        log::get()->warn("Invalid state : {}", status);
        return 1301;
    }
}

const std::vector<std::string> kAppAndSpeaker{service::clientcomm, service::speaker};

}  // namespace

// Forwards engine callbacks to the controller (the engine holds it weakly).
class PlaybackController::EngineDelegate : public media::PlaybackEngineGeneric::Delegate {
public:
    explicit EngineDelegate(PlaybackController& c) : c_(c) {}
    void onPrepared(std::uint32_t seq, const media::AudioItem& item) override { c_.onPrepared(seq, item); }
    void onStatusUpdated(const media::PlayStatus& status, const media::AudioItem&) override { c_.onStatusUpdated(status); }
    void onProgressUpdated(std::uint64_t pos, std::uint64_t dur) override { c_.onProgressUpdated(pos, dur); }
    void onAudioFormatUpdated(const media::AudioFormat& format) override { c_.onAudioFormatUpdated(format); }
    void onPlaybackCompleted(std::uint32_t seq, bool hasNext) override { c_.onPlaybackCompleted(seq, hasNext); }
    void onPrepareError(std::uint32_t seq, Engine::RecoveryAction a, const util::HttpResponse& r) override {
        c_.onPrepareError(seq, a, r);
    }
    void onPrepareNextError(std::uint32_t seq, Engine::RecoveryAction a, const util::HttpResponse& r) override {
        c_.onPrepareNextError(seq, a, r);
    }
    void onPlaybackError(std::uint32_t seq, const media::PlaybackError& e) override { c_.onPlaybackError(seq, e); }
    void onRequestNextMedia(std::uint32_t seq) override { c_.onRequestNextMedia(seq); }
    void onPreparedNext(std::uint32_t seq, const media::AudioItem& item) override { c_.onPreparedNext(seq, item); }

private:
    PlaybackController& c_;
};

PlaybackController::PlaybackController(std::shared_ptr<media::IMediaPlayer> player)
    : Service(service::playback),
      player_(std::move(player)),
      delegate_(std::make_shared<EngineDelegate>(*this)),
      defaultEngine_(std::make_shared<media::PlaybackEngineGeneric>()) {}

PlaybackController::~PlaybackController() {
    finish();
}

void PlaybackController::registerPlaybackEngine(const std::string& appId, const std::shared_ptr<Engine>& engine) {
    engines_[appId] = engine;
}

std::shared_ptr<PlaybackController::Engine> PlaybackController::lookupEngine(const std::string& appId) const {
    auto it = engines_.find(appId);
    return it == engines_.end() ? defaultEngine_ : it->second;
}

void PlaybackController::start() {
    if (player_ && player_->init() != media::kSuccess)
        log::get()->error("player init failed. {}", -1);
    Service::start();
}

void PlaybackController::finish() {
    if (!loopRunning() && state_ == State::NoEngine)
        return;
    Service::finish();
    updateState(State::NoEngine);
    mediaSeqNo_ = 0;
    currentMedia_ = nextMedia_ = Json();
    pendingSeek_ = 0;
    if (activeEngine_) {
        activeEngine_->reset();
        activeEngine_->disconnect();
        activeEngine_.reset();
    }
    if (player_ && player_->deinit() != media::kSuccess)
        log::get()->error("player deinit failed. {}", -1);
}

void PlaybackController::updateState(State state) {
    const State old = state_.exchange(state);
    if (old == state)
        log::get()->debug("same state:{}", stateName(state));
    else
        log::get()->debug("updateState:{}->{}", stateName(old), stateName(state));
}

void PlaybackController::updatePlayState(PlayState state) {
    const PlayState old = playState_.exchange(state);
    if (old == state)
        log::get()->debug("same state:{}", static_cast<int>(state));
    else
        log::get()->debug("updatePlayState:{}->{}", static_cast<int>(old), static_cast<int>(state));
}

void PlaybackController::handleMessage(std::uint32_t id, const Json& j) {
    switch (id) {
    case msg::PlaybackPlay: procPlay(j); break;
    case msg::PlaybackPause: procPause(j); break;
    case msg::PlaybackStop: procStop(j); break;
    case msg::PlaybackSeek: procSeek(j); break;
    case msg::PlaybackReset: procReset(); break;
    case msg::PlaybackSetServerInfo: procSetServerInfo(j); break;
    case msg::PlaybackSetMedia: procSetMedia(j); break;
    case msg::PlaybackSetNextMedia: procSetNextMedia(j); break;
    case msg::PlaybackInterruptSpeaker: procInterruptSpeaker(j); break;
    case msg::PlaybackNotifyPrepared: procNotifyPrepared(j); break;
    case msg::PlaybackNotifyPreparedNext: procNotifyPreparedNext(j); break;
    case msg::PlaybackNotifyMpCompleted: procNotifyMpCompleted(j); break;
    case msg::PlaybackNotifyMpRequestNextMedia: procNotifyMpRequestNextMedia(j); break;
    case msg::NotifySessionState: procNotifySessionState(j); break;
    case msg::RequestStatusNotification: procRequestStatusNotification(); break;
    default: log::get()->warn("playback: unhandled message 0x{:08x}", id); break;
    }
}

// --- Engine callbacks: hop onto the playback thread or update atomics ----------------

void PlaybackController::onPrepared(std::uint32_t seq, const media::AudioItem& item) {
    Json j{{"mediaSeqNo", seq}};
    if (!item.customData.empty())
        j["customData"] = item.customData;
    postMessage(msg::PlaybackNotifyPrepared, j);
}

void PlaybackController::onPreparedNext(std::uint32_t seq, const media::AudioItem& item) {
    Json j{{"mediaSeqNo", seq}};
    if (!item.customData.empty())
        j["customData"] = item.customData;
    postMessage(msg::PlaybackNotifyPreparedNext, j);
}

void PlaybackController::onPlaybackCompleted(std::uint32_t seq, bool hasNext) {
    postMessage(msg::PlaybackNotifyMpCompleted, {{"mediaSeqNo", seq}, {"hasNextMedia", hasNext}});
}

void PlaybackController::onRequestNextMedia(std::uint32_t seq) {
    postMessage(msg::PlaybackNotifyMpRequestNextMedia, {{"mediaSeqNo", seq}});
}

void PlaybackController::onStatusUpdated(const media::PlayStatus& status) {
    playerState_ = status.state;
    if (state_ != State::Preparing) {
        // Keep the requested play/pause in sync with changes made on the device side.
        if (status.state == media::playstate_t::Playing && playState_ == PlayState::Pause)
            updatePlayState(PlayState::Play);
        else if (status.state == media::playstate_t::Paused && playState_ == PlayState::Play)
            updatePlayState(PlayState::Pause);
    }
    if (state_ == State::Started)
        appNotifyPlayerStatusChanged();
}

void PlaybackController::onProgressUpdated(std::uint64_t positionMs, std::uint64_t durationMs) {
    progress_ = positionMs;
    duration_ = durationMs;
    if (state_ == State::Started)
        appNotifyPlayerStatusChanged();
}

void PlaybackController::onAudioFormatUpdated(const media::AudioFormat& f) {
    sendMessage(service::speaker, msg::SpeakerNotifyAudioFormatUpdate,
                {{"command", "notifyAudioFormatUpdated"},
                 {"mediaformat", f.mediaformat},
                 {"audioCodec", f.audioCodec},
                 {"streamType", f.streamType},
                 {"audioChannels", f.audioChannels},
                 {"audioBitPerSample", f.audioBitPerSample},
                 {"audioSamplingRate", f.audioSamplingRate},
                 {"audioBitrate", f.audioBitrate}});
}

void PlaybackController::onPrepareError(std::uint32_t seq, Engine::RecoveryAction action,
                                        const util::HttpResponse& response) {
    if (action == Engine::RecoveryAction::Default) {
        postMessage(msg::PlaybackStop, {{"mediaSeqNo", seq}, {"isNextMedia", false}});
        appNotifyContentServerError(response);
        queueSend(msg::QueueNotifyTokenExpired, {{"isNextMedia", false}});
    } else if (action == Engine::RecoveryAction::SkipTrack) {
        queueSend(msg::QueueNext, {{"mediaSeqNo", seq}});
    }
}

void PlaybackController::onPrepareNextError(std::uint32_t, Engine::RecoveryAction action, const util::HttpResponse&) {
    if (action == Engine::RecoveryAction::Default)
        queueSend(msg::QueueNotifyTokenExpired, {{"isNextMedia", true}});
}

void PlaybackController::onPlaybackError(std::uint32_t seq, const media::PlaybackError& error) {
    if (state_ == State::NoEngine || state_ == State::Idle)
        log::get()->error("[ASSERT] onPlaybackError: state_ != NO_ENGINE && state_ != IDLE");
    if (seq != mediaSeqNo_) {
        log::get()->info("old notify. wait={}, recv={}", mediaSeqNo_.load(), seq);
        return;
    }
    postMessage(msg::PlaybackStop, {{"mediaSeqNo", seq}, {"isNextMedia", false}});
    appNotifyPlaybackError(error);
}

// --- Commands ------------------------------------------------------------------------

void PlaybackController::procPlay(const Json& j) {
    if (state_ == State::NoEngine || state_ == State::Idle) {
        log::get()->warn("Illegal state: {}", stateName(state_));
        appNotifyRequestResult(j, kResultIllegalState);
        return;
    }
    if (playState_ != PlayState::Play) {
        updatePlayState(PlayState::Play);
        if (state_ == State::Started)
            activeEngine_->play();
        else
            appNotifyPlayerStatusChanged();
    }
    appNotifyRequestResult(j, kResultSuccess);
}

void PlaybackController::procPause(const Json& j) {
    if (state_ == State::NoEngine || state_ == State::Idle) {
        log::get()->warn("Illegal state: {}", stateName(state_));
        appNotifyRequestResult(j, kResultIllegalState);
        return;
    }
    if (playState_ != PlayState::Pause) {
        updatePlayState(PlayState::Pause);
        if (state_ == State::Started)
            activeEngine_->pause();
        else
            appNotifyPlayerStatusChanged();
    }
    appNotifyRequestResult(j, kResultSuccess);
}

void PlaybackController::procStop(const Json& j) {
    const State s = state_;
    if (s == State::NoEngine || s == State::Idle) {
        log::get()->warn("Illegal state: {}", stateName(s));
        appNotifyRequestResult(j, kResultIllegalState);
        return;
    }
    if (s != State::Stopped) {
        mediaSeqNo_ = 0;
        updateState(State::Stopped);
        activeEngine_->stop();
        appNotifyPlayerStatusChanged();
        currentMedia_ = nextMedia_ = Json();
        pendingSeek_ = 0;
    }
    appNotifyRequestResult(j, kResultSuccess);
}

void PlaybackController::procSeek(const Json& j) {
    const auto position = static_cast<std::uint64_t>(std::max(0.0, jv::number(jv::at(j, "packet"), "position")));
    switch (state_.load()) {
    case State::Started:
    case State::Completed:
        activeEngine_->seekTo(position);
        progress_ = position;  // as in the Preparing case below
        break;
    case State::Preparing:
        pendingSeek_ = position;
        // Store the target now, so status updates sent before the player reports again
        // (BUFFERING, play after a paused seek) don't move the app's bar back.
        progress_ = position;
        break;
    default:
        log::get()->warn("Illegal state: {}", stateName(state_));
        appNotifyRequestResult(j, kResultIllegalState);
        return;
    }
    appNotifyRequestResult(j, kResultSuccess);
}

void PlaybackController::resetMedia() {
    currentMedia_ = nextMedia_ = Json();
    pendingSeek_ = 0;
    playerState_ = media::playstate_t::Idle;
    progress_ = 0;
    duration_ = 0;
    serverInfo_.reset();
}

void PlaybackController::procReset() {
    if (state_ == State::NoEngine || state_ == State::Idle) {
        log::get()->warn("Illegal state: {}", stateName(state_));
        return;
    }
    updateState(State::Idle);
    updatePlayState(PlayState::None);
    mediaSeqNo_ = 0;
    activeEngine_->reset();
    resetMedia();
    appNotifyPlayerStatusChanged();
}

void PlaybackController::procSetServerInfo(const Json& j) {
    serverInfo_ = media::ServerInfo::fromJson(j);
}

bool PlaybackController::hasPlayable(const Json& mediaInfo) const {
    return serverInfo_ ? jv::at(mediaInfo, "mediaId").is_string() : jv::at(mediaInfo, "srcUrl").is_string();
}

void PlaybackController::prepare(const Json& mediaInfo, bool next) {
    const std::uint32_t seq = mediaSeqNo_;
    if (serverInfo_) {
        const auto mediaId = jv::string(mediaInfo, "mediaId");
        const auto type = static_cast<unsigned>(jv::integer(mediaInfo, "mediaType"));
        if (next)
            activeEngine_->prepareNext(seq, *serverInfo_, mediaId, type);
        else
            activeEngine_->prepare(seq, *serverInfo_, mediaId, type);
    } else {
        const auto url = jv::string(mediaInfo, "srcUrl");
        if (next)
            activeEngine_->prepareNext(seq, url);
        else
            activeEngine_->prepare(seq, url);
    }
}

void PlaybackController::procSetMedia(const Json& j) {
    if (state_ == State::NoEngine) {
        log::get()->error("[ASSERT] procSetMedia: state NO_ENGINE");
        return;  // no engine to play with
    }
    mediaSeqNo_ = static_cast<std::uint32_t>(jv::number(j, "mediaSeqNo"));
    if (state_ == State::Preparing || state_ == State::Started)
        activeEngine_->stop();
    const Json& mediaInfo = jv::at(j, "mediaInfo");
    if (!jv::at(mediaInfo, "metadata").is_object() || !hasPlayable(mediaInfo)) {
        appNotifyPlaybackError({media::track_status::kInvalidMedia, 0, {}});
        return;
    }
    currentMedia_ = mediaInfo;
    nextMedia_ = Json();
    pendingSeek_ = static_cast<std::uint64_t>(std::max(0, jv::integer(j, "position")));
    progress_ = pendingSeek_;
    duration_ = static_cast<std::uint64_t>(std::max(0, jv::integer(jv::at(mediaInfo, "metadata"), "duration")));
    appNotifyMediaChanged(currentMedia_);
    updateState(State::Preparing);
    if (playState_ == PlayState::None)
        updatePlayState(PlayState::Pause);
    appNotifyPlayerStatusChanged();
    prepare(currentMedia_, false);
}

void PlaybackController::procNotifyPrepared(const Json& j) {
    const auto seq = static_cast<std::uint32_t>(jv::number(j, "mediaSeqNo"));
    if (state_ != State::Preparing || seq != mediaSeqNo_) {
        log::get()->debug("This message is old. state_={}, wait={}, recv={}", stateName(state_), mediaSeqNo_.load(), seq);
        return;
    }
    updateState(State::Started);
    if (const auto custom = jv::string(j, "customData"); !custom.empty()) {
        currentMedia_["customData"] = custom;
        appNotifyMediaChanged(currentMedia_);
    }
    if (pendingSeek_ != 0) {
        activeEngine_->seekTo(pendingSeek_);
        pendingSeek_ = 0;
    }
    if (playState_ == PlayState::Play)
        activeEngine_->play();
    else if (playState_ == PlayState::Pause)
        activeEngine_->pause();
}

void PlaybackController::procSetNextMedia(const Json& j) {
    const auto seq = static_cast<std::uint32_t>(jv::number(j, "mediaSeqNo"));
    if (state_ != State::Started || seq != mediaSeqNo_) {
        log::get()->debug("This message is old. state_={}, wait={}, recv={}", stateName(state_), mediaSeqNo_.load(), seq);
        return;
    }
    const Json& mediaInfo = jv::at(j, "mediaInfo");
    if (!hasPlayable(mediaInfo)) {
        appNotifyPlaybackError({media::track_status::kInvalidMedia, 0, {}});
        return;
    }
    nextMedia_ = mediaInfo;
    prepare(nextMedia_, true);
}

void PlaybackController::procNotifyPreparedNext(const Json& j) {
    const auto seq = static_cast<std::uint32_t>(jv::number(j, "mediaSeqNo"));
    if (state_ != State::Started || seq != mediaSeqNo_)
        return;
    if (const auto custom = jv::string(j, "customData"); !custom.empty() && nextMedia_.is_object())
        nextMedia_["customData"] = custom;
}

void PlaybackController::procNotifyMpRequestNextMedia(const Json& j) {
    const auto seq = static_cast<std::uint32_t>(jv::number(j, "mediaSeqNo"));
    if (state_ != State::Started || seq != mediaSeqNo_)
        return;
    queueSend(msg::QueueRequestNextMedia, j);
}

void PlaybackController::procNotifyMpCompleted(const Json& j) {
    const auto seq = static_cast<std::uint32_t>(jv::number(j, "mediaSeqNo"));
    if (state_ != State::Started || seq != mediaSeqNo_) {
        log::get()->debug("This message is old. state_={}, wait={}, recv={}", stateName(state_), mediaSeqNo_.load(), seq);
        return;
    }
    queueSend(msg::QueueNotifyMediaEnded, j);
    if (jv::boolean(j, "hasNextMedia")) {
        currentMedia_ = nextMedia_;
        nextMedia_ = Json();
        appNotifyMediaChanged(currentMedia_);
    } else {
        mediaSeqNo_ = 0;
        updateState(State::Completed);
        appNotifyPlayerStatusChanged();
    }
}

void PlaybackController::procInterruptSpeaker(const Json& j) {
    if (state_ == State::NoEngine) {
        log::get()->warn("Illegal state: {}", stateName(state_));
        return;
    }
    const bool on = jv::boolean(jv::at(j, "packet"), "interrupt_on");
    activeEngine_->interrupt(on);
    sendMessage(service::speaker, msg::SendPacket,
                {{"packet", {{"command", "notifyInterrupted"}, {"interrupt_on", on}}}});
}

void PlaybackController::procNotifySessionState(const Json& j) {
    const int state = jv::integer(j, "state");
    if (state == 0) {
        if (state_ == State::NoEngine)
            return;
        if (state_ != State::Idle)
            procReset();
        activeEngine_->disconnect();
        activeEngine_.reset();
        updateState(State::NoEngine);
        return;
    }
    if (state != 2)
        return;
    if (state_ != State::NoEngine) {
        if (state_ != State::Idle)
            procReset();
        activeEngine_->disconnect();  // the casting app changed: switch engines
    }
    const std::string appId = jv::string(jv::at(j, "appInfo"), "appId");
    updateState(State::Idle);
    activeEngine_ = lookupEngine(appId);
    activeEngine_->connect(player_, delegate_);
    activeEngine_->enableServerVerification(true);
}

void PlaybackController::procRequestStatusNotification() {
    if (state_ == State::NoEngine)
        return;
    appNotifyMediaChanged(currentMedia_);
    appNotifyPlayerStatusChanged();
}

// --- Outbound -------------------------------------------------------------------------

void PlaybackController::appNotifyPlayerStatusChanged() {
    std::lock_guard lock(notifyMutex_);
    std::string playerState;
    switch (state_.load()) {
    case State::NoEngine:
    case State::Idle: playerState = "IDLE"; break;
    case State::Started: playerState = media::toString(playerState_.load()); break;
    case State::Completed: playerState = "PAUSED"; break;
    case State::Preparing:
    case State::Stopped: playerState = playState_ == PlayState::Play ? "BUFFERING" : "PAUSED"; break;
    }
    const bool idle = playerState == "IDLE";
    sendMessage(kAppAndSpeaker, msg::SendPacket,
                {{"packet", {{"command", "notifyPlayerStatusChanged"},
                             {"playerState", playerState},
                             {"progress", idle ? 0 : progress_.load()},
                             {"duration", idle ? 0 : duration_.load()}}}});
}

void PlaybackController::appNotifyMediaChanged(const Json& mediaInfo) {
    if (mediaInfo.is_null()) {
        log::get()->debug("notifyMediaChanged not sent: no media info");
        return;
    }
    sendMessage(kAppAndSpeaker, msg::SendPacket,
                {{"packet", {{"command", "notifyMediaChanged"}, {"mediaInfo", mediaInfo}}}});
}

void PlaybackController::appNotifyPlaybackError(const media::PlaybackError& error) {
    Json packet{{"command", "notifyPlaybackError"}, {"errorCode", convertTrackStatus(error.trackStatus)}};
    if (error.httpStatus != 0)
        packet["details"] = {{"subCode", error.httpStatus}, {"description", error.description}};
    sendMessage(kAppAndSpeaker, msg::SendPacket, {{"packet", packet}});
}

void PlaybackController::appNotifyContentServerError(const util::HttpResponse& response) {
    Json details{{"subCode", response.status}};
    if (!response.body.empty())
        details["description"] = response.bodyString();
    sendMessage(service::clientcomm, msg::SendPacket,
                {{"packet", {{"command", "notifyContentServerError"}, {"errorCode", kErrorContentServer}, {"details", details}}}});
}

void PlaybackController::appNotifyRequestResult(const Json& req, int code) {
    const Json& requestId = jv::at(jv::at(req, "packet"), "requestId");
    const Json& socketId = jv::at(req, "socketId");
    if (!requestId.is_number() || !socketId.is_number())
        return;
    sendMessage(jv::number(socketId) == 0 ? service::speaker : service::clientcomm, msg::SendPacket,
                {{"socketId", socketId},
                 {"packet", {{"command", "notifyRequestResult"}, {"requestId", requestId}, {"resultCode", code}}}});
}

void PlaybackController::queueSend(std::uint32_t id, const Json& payload) {
    sendMessage(service::queue, id, payload);
}

}  // namespace tcng::core
