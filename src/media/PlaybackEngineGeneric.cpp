#include "media/PlaybackEngineGeneric.hpp"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

#include "util/Log.hpp"

namespace tcng::media {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

const nlohmann::json& member(const nlohmann::json& j, const char* key) {
    static const nlohmann::json kNull;
    if (!j.is_object())
        return kNull;
    auto it = j.find(key);
    return it == j.end() ? kNull : *it;
}

std::string stringValue(const nlohmann::json& j) {
    return j.is_string() ? j.get<std::string>() : std::string();
}

std::vector<std::string> stringArray(const nlohmann::json& j) {
    std::vector<std::string> out;
    if (j.is_array())
        for (const auto& v : j)
            out.push_back(stringValue(v));
    return out;
}

std::map<std::string, std::string> lowerKeyedObject(const nlohmann::json& j) {
    std::map<std::string, std::string> out;
    if (j.is_object())
        for (const auto& [k, v] : j.items())
            out.emplace(toLower(k), stringValue(v));
    return out;
}

}  // namespace

ServerInfo ServerInfo::fromJson(const nlohmann::json& j) {
    ServerInfo si;
    const auto& authInfo = member(j, "authInfo");
    const auto& oauth = member(authInfo, "oauthServerInfo");
    const auto& oauthAuth = member(oauth, "authInfo");
    const auto& oauthParams = member(oauthAuth, "oauthParameters");
    si.serverUrl = stringValue(member(j, "serverUrl"));
    si.headerAuth = stringValue(member(authInfo, "headerAuth"));
    si.oauth.serverUrl = stringValue(member(oauth, "serverUrl"));
    si.oauth.headerAuth = stringValue(member(oauthAuth, "headerAuth"));
    si.oauth.accessToken = stringValue(member(oauthParams, "accessToken"));
    si.oauth.refreshToken = stringValue(member(oauthParams, "refreshToken"));
    si.oauth.httpHeaderFields = stringArray(member(oauth, "httpHeaderFields"));
    si.oauth.formParameters = lowerKeyedObject(member(oauth, "formParameters"));
    si.queryParameters = lowerKeyedObject(member(j, "queryParameters"));
    si.httpHeaderFields = stringArray(member(j, "httpHeaderFields"));
    return si;
}

PlaybackEngineGeneric::PlaybackEngineGeneric() : http_(std::make_shared<util::AsyncHttpClient>(1)) {}

PlaybackEngineGeneric::~PlaybackEngineGeneric() {
    http_->stop();
}

std::shared_ptr<PlaybackEngineGeneric::Delegate> PlaybackEngineGeneric::lockDelegate() const {
    std::lock_guard lock(delegateMutex_);
    return delegate_.lock();
}

int PlaybackEngineGeneric::connect(const std::shared_ptr<IMediaPlayer>& player, const std::shared_ptr<Delegate>& delegate) {
    {
        std::lock_guard lock(mutex_);
        player_ = player;
        if (player_)
            player_->connect(this);
    }
    {
        std::lock_guard lock(delegateMutex_);
        delegate_ = delegate;
    }
    http_->start();
    return kSuccess;
}

int PlaybackEngineGeneric::disconnect() {
    std::shared_ptr<IMediaPlayer> player;
    {
        std::lock_guard lock(mutex_);
        if (pendingTaskId_ != 0) {
            http_->cancel(pendingTaskId_);
            pendingTaskId_ = 0;
        }
        pendingIsNext_ = false;
        player.swap(player_);
    }
    // Stop the worker outside the engine mutex: its onReceived() waits for that mutex.
    http_->stop();
    if (player)
        player->disconnect();
    std::lock_guard lock(delegateMutex_);
    delegate_.reset();
    return kSuccess;
}

int PlaybackEngineGeneric::reset() {
    return stop();
}

void PlaybackEngineGeneric::notifyStopped(const std::shared_ptr<Delegate>& delegate) {
    if (!delegate)
        return;
    delegate->onStatusUpdated(PlayStatus{}, AudioItem{});
    delegate->onProgressUpdated(0, 0);
}

int PlaybackEngineGeneric::stop() {
    std::lock_guard lock(mutex_);
    if (!player_) {
        log::get()->error("[ASSERT] {}", "player_ != nullptr");
        return 1;
    }
    seq_ = 0;
    if (pendingTaskId_ != 0) {
        http_->cancel(pendingTaskId_);
        pendingTaskId_ = 0;
    }
    pendingIsNext_ = false;
    const int rc = player_->stop();
    notifyStopped(lockDelegate());
    return rc;
}

int PlaybackEngineGeneric::prepare(std::uint32_t seq, const std::string& url) {
    std::lock_guard lock(mutex_);
    if (!player_)
        return 1;
    seq_ = seq;
    AudioItem item;
    item.url = url;
    const int rc = player_->prepare(item);
    if (auto d = lockDelegate())
        d->onPrepared(seq, item);
    return rc;
}

int PlaybackEngineGeneric::prepareNext(std::uint32_t seq, const std::string& url) {
    std::lock_guard lock(mutex_);
    if (!player_)
        return 1;
    if (seq != seq_) {
        log::get()->debug("old media_seqno: now={}, in={}", seq_.load(), seq);
        return 1;
    }
    AudioItem item;
    item.url = url;
    const int rc = player_->prepareNext(item);
    if (auto d = lockDelegate())
        d->onPreparedNext(seq, item);
    return rc;
}

util::HttpRequest PlaybackEngineGeneric::buildUrlFetchRequest(const ServerInfo& si, const std::string& mediaId) {
    util::HttpRequest req;
    req.method = util::HttpMethod::Get;
    req.url = si.serverUrl;
    req.host = util::HttpClient::hostOf(si.serverUrl);
    req.query.emplace("media_id", mediaId);
    req.headers = si.httpHeaderFields;
    return req;
}

void PlaybackEngineGeneric::requestUrlFetch(const ServerInfo& si, const std::string& mediaId, bool isNext) {
    if (pendingTaskId_ != 0) {
        http_->cancel(pendingTaskId_);
        pendingIsNext_ = false;
    }
    pendingTaskId_ = http_->request(buildUrlFetchRequest(si, mediaId), shared_from_this());
    if (pendingTaskId_ == 0)
        log::get()->error("[ASSERT] {}", "pendingTaskId_ != 0");
    pendingIsNext_ = isNext;
    log::get()->debug("url fetch start: {}", pendingTaskId_);
}

int PlaybackEngineGeneric::prepare(std::uint32_t seq, const ServerInfo& si, const std::string& mediaId, unsigned) {
    std::lock_guard lock(mutex_);
    seq_ = seq;
    requestUrlFetch(si, mediaId, false);
    return kSuccess;
}

int PlaybackEngineGeneric::prepareNext(std::uint32_t seq, const ServerInfo& si, const std::string& mediaId, unsigned) {
    std::lock_guard lock(mutex_);
    if (seq != seq_) {
        log::get()->debug("old media_seqno: now={}, in={}", seq_.load(), seq);
        return 1;
    }
    requestUrlFetch(si, mediaId, true);
    return kSuccess;
}

void PlaybackEngineGeneric::onReceived(std::uint32_t id, const util::HttpResponse& response) {
    std::lock_guard lock(mutex_);
    if (id != pendingTaskId_ || !player_) {
        log::get()->debug("url fetch discard: {}", id);
        return;
    }
    const bool isNext = pendingIsNext_;
    pendingTaskId_ = 0;
    pendingIsNext_ = false;
    const std::uint32_t seq = seq_;
    auto delegate = lockDelegate();
    // Only a 200 body is a URL; an error page is reported as a prepare error.
    if (response.status != 200) {
        if (delegate) {
            if (isNext)
                delegate->onPrepareNextError(seq, RecoveryAction::Default, response);
            else
                delegate->onPrepareError(seq, RecoveryAction::Default, response);
        }
        return;
    }
    AudioItem item;
    item.url = response.bodyString();
    log::get()->debug("url fetch done: {}, {}", id, item.url);
    if (isNext) {
        player_->prepareNext(item);
        if (delegate)
            delegate->onPreparedNext(seq, item);
    } else {
        player_->prepare(item);
        if (delegate)
            delegate->onPrepared(seq, item);
    }
}

int PlaybackEngineGeneric::play() {
    std::lock_guard lock(mutex_);
    return player_ ? player_->play() : 1;
}

int PlaybackEngineGeneric::pause() {
    std::lock_guard lock(mutex_);
    return player_ ? player_->pause() : 1;
}

int PlaybackEngineGeneric::seekTo(std::uint64_t positionMs) {
    std::lock_guard lock(mutex_);
    return player_ ? player_->seekTo(positionMs) : 1;
}

int PlaybackEngineGeneric::clearToken() {
    return kSuccess;
}

int PlaybackEngineGeneric::enableServerVerification(bool enable) {
    http_->enableServerVerification(enable);
    if (!enable)
        log::get()->warn("enableServerVerification(false) ignored: TLS verification is always on");
    return kSuccess;
}

int PlaybackEngineGeneric::interrupt(bool on) {
    std::lock_guard lock(mutex_);
    return player_ ? player_->interrupt(on) : 1;
}

// --- IMediaPlayer::Delegate -> PlaybackEngineGeneric::Delegate ---

void PlaybackEngineGeneric::onStatusUpdated(const PlayStatus& status, const AudioItem& item) {
    if (auto d = lockDelegate())
        d->onStatusUpdated(status, item);
}

void PlaybackEngineGeneric::onProgressUpdated(std::uint64_t positionMs, std::uint64_t durationMs) {
    if (auto d = lockDelegate())
        d->onProgressUpdated(positionMs, durationMs);
}

void PlaybackEngineGeneric::onAudioFormatUpdated(const AudioFormat& format) {
    if (auto d = lockDelegate())
        d->onAudioFormatUpdated(format);
}

void PlaybackEngineGeneric::onPlaybackCompleted(bool hasNext) {
    if (auto d = lockDelegate())
        d->onPlaybackCompleted(seq_, hasNext);
}

void PlaybackEngineGeneric::onPlaybackError(const PlaybackError& error) {
    if (auto d = lockDelegate())
        d->onPlaybackError(seq_, error);
}

void PlaybackEngineGeneric::onRequestNextMedia() {
    if (auto d = lockDelegate())
        d->onRequestNextMedia(seq_);
}

}  // namespace tcng::media
