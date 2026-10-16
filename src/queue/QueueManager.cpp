#include "queue/QueueManager.hpp"

#include <algorithm>
#include <functional>

#include "util/JsonUtil.hpp"
#include "core/MessageIds.hpp"
#include "util/HttpClient.hpp"
#include "util/Log.hpp"

namespace tcng::queue {

using core::Json;
namespace msg = core::msg;
namespace svc = core::service;

namespace {

constexpr int kResultSuccess = 0;
constexpr int kResultNoMoreItems = 1001;
constexpr int kResultIllegalState = 1002;
constexpr int kResultSuperseded = 1003;
constexpr int kResultCancelled = 1004;
constexpr int kResultInvalidItems = 1097;
constexpr int kResultIllegalReport = 1099;

const char* stateName(QueueManager::State s) {
    switch (s) {
    case QueueManager::State::Idle: return "IDLE";
    case QueueManager::State::Loading: return "LOADING";
    case QueueManager::State::Loaded: return "LOADED";
    case QueueManager::State::Moving: return "MOVING";
    }
    return "?";
}

}  // namespace

// --- Delegates (forward window/loader callbacks onto the queue thread) -----------

class QueueManager::WindowDelegate : public QueueWindow::Delegate {
public:
    explicit WindowDelegate(QueueManager& m) : m_(m) {}
    void onError() override {}
    // Called from window navigation, which runs on the queue thread.
    void onRequestItemDelete() override { m_.requestItemDelete_ = true; }

private:
    QueueManager& m_;
};

class QueueManager::LoaderDelegate : public QueueWindowLoader::Delegate {
public:
    explicit LoaderDelegate(QueueManager& m) : m_(m) {}
    void onRequestQueueItems(const std::shared_ptr<QueueWindow>& window) override { m_.appRequestQueueItems(window); }
    void onLoaded(const std::shared_ptr<QueueWindow>& window) override {
        m_.postMessage(msg::QueueNotifyLoaded, {{"queueId", window->getQueueId()}});
    }
    void onQueueItemsChanged(QueueWindowLoader::Reason reason) override {
        m_.postMessage(msg::QueueNotifyQueueItemsChanged, {{"reason", static_cast<int>(reason)}});
    }
    void onLoadError(QueueWindowLoader::RecoveryAction action, const util::HttpResponse& resp) override {
        m_.postMessage(msg::QueueNotifyLoadError, {{"recovery_action", static_cast<int>(action)},
                                                   {"status_code", resp.status},
                                                   {"response_message", resp.bodyString()}});
    }

private:
    QueueManager& m_;
};

// --- Lifecycle ----------------------------------------------------------------------

QueueManager::QueueManager()
    : Service(svc::queue),
      windowDelegate_(std::make_shared<WindowDelegate>(*this)),
      loaderDelegate_(std::make_shared<LoaderDelegate>(*this)) {}

QueueManager::~QueueManager() {
    finish();
}

void QueueManager::start() {
    log::get()->info("## QueueManager Config ##");
    if (!loader_) {
        loader_ = std::make_shared<QueueWindowLoader>(loaderDelegate_);
        loader_->enableServerVerification(true);
    }
    Service::start();
}

void QueueManager::finish() {
    Service::finish();
    if (loader_) {
        loader_->reset();
        loader_.reset();
    }
    active_.reset();
    loading_.reset();
    setState(State::Idle);
    util::HttpClient::globalCloseAllKeepalive();
}

void QueueManager::setState(State state) {
    if (state_ != state)
        log::get()->debug(" state {}->{}", stateName(state_), stateName(state));
    state_ = state;
}

void QueueManager::handleMessage(std::uint32_t id, const Json& j) {
    switch (id) {
    case msg::QueueLoadMedia: break;  // no-op
    case msg::QueueLoadLocalQueue:
    case msg::QueueLoadCloudQueue: loadQueueWindow(j); break;
    case msg::QueueRespondQueueItems: procRespondQueueItems(j); break;
    case msg::QueuePrevious: procPrevious(j); break;
    case msg::QueueNext: procNext(j); break;
    case msg::QueueSelectQueueItem: procSelectQueueItem(j); break;
    case msg::QueueRefreshQueue: procRefreshQueue(j); break;
    case msg::QueueSetRepeatMode: procSetRepeatMode(j); break;
    case msg::QueueSetShuffle: procSetShuffle(j); break;
    case msg::QueueUpdateServerInfo: procUpdateServerInfo(j); break;
    case msg::QueuePurge: procPurgeQueue(); break;
    case msg::QueueNotifyMediaEnded: procNotifyMediaEnded(j); break;
    case msg::QueueRequestNextMedia: procRequestNextMedia(j); break;
    case msg::QueueNotifyTokenExpired: procNotifyTokenExpired(j); break;
    case msg::QueueNotifyLoaded: procNotifyLoaded(j); break;
    case msg::QueueNotifyLoadError: procNotifyLoadError(j); break;
    case msg::QueueNotifyQueueItemsChanged: procNotifyQueueItemsChanged(j); break;
    case msg::RequestStatusNotification: procRequestStatusNotification(j); break;
    default: log::get()->warn("handleMessage command:0x{:08x}", id); break;
    }
}

// --- Load --------------------------------------------------------------------------

void QueueManager::saveLoadRequest(const Json& j) {
    loadRequest_ = j;
    loadRequestSocketId_ = static_cast<std::uint32_t>(jv::number(j, "socketId"));
}

void QueueManager::loadQueueWindow(const Json& j) {
    const Json& packet = jv::at(j, "packet");
    setState(State::Loading);
    resetPlaybackMedia();
    cancelLoading();
    active_.reset();
    loading_.reset();
    reloadOnServerInfo_ = tokenExpired_ = tokenExpiredNext_ = false;

    if (const Json& content = jv::at(packet, "contentServerInfo"); !content.is_null())
        playbackSetServerInfo(content);

    const Json& current = jv::at(packet, "currentMediaInfo");
    ++mediaSeqNo_;
    setPlaybackMedia(current, true, jv::integer(packet, "position"));
    if (jv::boolean(packet, "autoplay"))
        playbackPlay();
    saveLoadRequest(j);

    const Json& info = jv::at(packet, "queueInfo");
    auto size = [&](const char* key) {
        const Json& v = jv::at(info, key);
        return v.is_number() ? std::min<unsigned>(static_cast<unsigned>(std::max(0, jv::integer(v))), kMaxWindowSize)
                             : kMaxWindowSize;
    };
    loading_ = std::make_shared<QueueWindow>(jv::string(info, "queueId"),
                                             repeatModeFromString(jv::string(info, "repeatMode")),
                                             jv::boolean(info, "shuffled"), size("maxBeforeSize"),
                                             size("maxAfterSize"), QueueItem(current, true));
    loading_->setDelegate(windowDelegate_);
    const Json& queueServer = jv::at(packet, "queueServerInfo");
    log::get()->info("[{}] <load> mode:{}/{}, req_num:{}/{}, qserver={}, cserver={}", loading_->getId(),
                     repeatModeToString(loading_->getRepeatMode()), loading_->isShuffled(),
                     loading_->getMaxBeforeSize(), loading_->getMaxAfterSize(), !queueServer.is_null(),
                     !jv::at(packet, "contentServerInfo").is_null());

    loaderDelegate_->onQueueItemsChanged(QueueWindowLoader::Reason::RepeatModeChanged);
    loaderDelegate_->onQueueItemsChanged(QueueWindowLoader::Reason::ShuffleChanged);
    appNotifyQueueChanged(loading_, 0);
    if (!queueServer.is_null())
        loader_->setServerInfo(parseServerInfo(queueServer));
    loader_->load(loading_);
}

void QueueManager::procRespondQueueItems(const Json& j) {
    if (state_ != State::Loading && state_ != State::Moving) {
        log::get()->warn("Illegal state: {}", stateName(state_));
        appNotifyRequestResult(j, kResultIllegalState);
        return;
    }
    const Json& packet = jv::at(j, "packet");
    if (!loading_ || static_cast<std::uint32_t>(jv::number(packet, "queueRequestId")) != loading_->getId()) {
        appNotifyRequestResult(j, kResultCancelled);
        return;
    }
    const Json& mediaInfos = jv::at(packet, "mediaInfos");
    if (!mediaInfos.is_array()) {
        appNotifyRequestResult(j, kResultInvalidItems);
        return;
    }
    std::vector<QueueItem> items;
    for (const auto& m : mediaInfos)
        items.emplace_back(m, true);
    loader_->respondQueueItems(items, jv::boolean(packet, "hasBegin"), jv::boolean(packet, "hasEnd"));
    appNotifyRequestResult(j, kResultSuccess);
}

// --- Navigation ---------------------------------------------------------------------

void QueueManager::startMoving() {
    setState(State::Moving);
    loading_ = active_->getNewWindow();
    loader_->load(loading_);
}

void QueueManager::navigate(const Json& j, bool forward) {
    const std::uint32_t id = forward ? msg::QueueNext : msg::QueuePrevious;
    switch (state_) {
    case State::Idle:
        appNotifyRequestResult(j, kResultIllegalState);
        return;
    case State::Loading:
        playbackStop();
        pending_.push_back({id, j});
        return;
    case State::Loaded: {
        Report report;
        auto item = forward ? active_->next(report) : active_->previous(report);
        if (item) {
            ++mediaSeqNo_;
            setPlaybackMedia(*item, true);
        }
        if (report == Report::OkPrefetch)
            startMoving();
        appNotifyRequestResult(j, kResultSuccess);
        return;
    }
    case State::Moving: {
        // The next window is still loading; navigate within the active one.
        Report report;
        auto item = forward ? active_->next(report) : active_->previous(report);
        if (item) {
            ++mediaSeqNo_;
            setPlaybackMedia(*item, false);
        }
        const Report edge = forward ? Report::End : Report::Begin;
        if (report == Report::Ok || report == Report::OkPrefetch || report == edge) {
            appNotifyRequestResult(j, kResultSuccess);
        } else if (report == Report::Exhausted) {
            playbackStop();
            pending_.push_back({id, j});
        } else {
            log::get()->critical("[ASSERT] navigate: report {}", static_cast<int>(report));
            checkReport(report, j, true);
        }
        return;
    }
    }
}

void QueueManager::procNext(const Json& j) {
    navigate(j, true);
}

void QueueManager::procPrevious(const Json& j) {
    navigate(j, false);
}

void QueueManager::procSelectQueueItem(const Json& j) {
    if (state_ == State::Idle) {
        log::get()->warn("Invalid state:IDLE");
        appNotifyRequestResult(j, kResultIllegalState);
        return;
    }
    const Json& mediaInfo = jv::at(jv::at(j, "packet"), "mediaInfo");
    const std::string itemId = jv::string(mediaInfo, "itemId");
    ++mediaSeqNo_;
    setPlaybackMedia(mediaInfo, true, 0);

    bool reload = false;
    if (state_ == State::Loading || state_ == State::Moving) {
        loader_->cancel();
        if (state_ == State::Loading) {
            if (!loadRequest_.is_null())
                appNotifyRequestResult(loadRequest_, kResultSuperseded);
            loadRequest_ = Json();
        } else {
            loading_.reset();
        }
        cancelPendingMessages();
        reload = (state_ == State::Loading);
    }
    if (!reload && active_) {
        Report report;
        if (active_->select(itemId, report)) {
            if (requestItemDelete_) {
                loader_->deleteItem(active_->getQueueId(), itemId);
                requestItemDelete_ = false;
            }
            setState(State::Loaded);
            appNotifyRequestResult(j, kResultSuccess);
            if (report == Report::OkPrefetch)
                startMoving();
            return;
        }
    }
    // Reload a window centred on the selected item; answered when it has loaded.
    setState(State::Loading);
    saveLoadRequest(j);
    const QueueItem selected(mediaInfo, true);
    if (active_) {
        loading_ = active_->getNewWindow(selected);
        active_.reset();
    } else if (loading_) {
        loading_ = loading_->getNewWindow(selected);
    }
    if (!loading_)
        return;
    if (requestItemDelete_) {
        loader_->deleteItem(loading_->getQueueId(), itemId);
        requestItemDelete_ = false;
    }
    loader_->load(loading_);
}

void QueueManager::procRefreshQueue(const Json& j) {
    const auto socketId = static_cast<std::uint32_t>(jv::number(j, "socketId"));
    switch (state_) {
    case State::Idle:
        appNotifyRequestResult(j, kResultIllegalState);
        return;
    case State::Loading:
        loader_->cancel();
        if (!loadRequest_.is_null())
            appNotifyRequestResult(loadRequest_, kResultSuperseded);
        cancelPendingMessages();
        saveLoadRequest(j);
        loading_ = loading_->getNewWindow();
        break;
    case State::Loaded:
    case State::Moving:
        if (state_ == State::Moving) {
            loader_->cancel();
            cancelPendingMessages();
        }
        setState(State::Loading);
        saveLoadRequest(j);
        loading_ = active_->getNewWindow();
        active_.reset();
        break;
    }
    appNotifyQueueItemsChanged(loading_, "REFRESH_REQUESTED", socketId);
    loader_->load(loading_);
}

void QueueManager::reloadWith(const Json& j, const std::function<void(const std::shared_ptr<QueueWindow>&)>& load) {
    if (state_ == State::Loading) {
        if (!loadRequest_.is_null())
            appNotifyRequestResult(loadRequest_, loading_ && loading_->getState() == QueueWindow::State::ItemsChanged
                                                     ? kResultSuccess
                                                     : kResultSuperseded);
        loader_->cancel();
        cancelPendingMessages();
        saveLoadRequest(j);
        loading_ = loading_->getNewWindow();
    } else {
        if (state_ == State::Moving) {
            loader_->cancel();
            cancelPendingMessages();
        }
        setState(State::Loading);
        saveLoadRequest(j);
        loading_ = active_->getNewWindow();
        active_.reset();
    }
    load(loading_);
}

void QueueManager::procSetShuffle(const Json& j) {
    if (state_ == State::Idle) {
        appNotifyRequestResult(j, kResultIllegalState);
        return;
    }
    const bool shuffle = jv::boolean(jv::at(j, "packet"), "shuffle");
    if (state_ != State::Loading && active_->isShuffled() == shuffle) {
        appNotifyRequestResult(j, kResultSuccess);
        return;
    }
    reloadWith(j, [&](const std::shared_ptr<QueueWindow>& w) {
        w->setShuffled(shuffle);
        loader_->loadWithShuffle(w, shuffle);
    });
}

void QueueManager::procSetRepeatMode(const Json& j) {
    if (state_ == State::Idle) {
        appNotifyRequestResult(j, kResultIllegalState);
        return;
    }
    const RepeatMode mode = repeatModeFromString(jv::string(jv::at(j, "packet"), "repeatMode"));
    if (state_ != State::Loading && active_->getRepeatMode() == mode) {
        appNotifyRequestResult(j, kResultSuccess);
        return;
    }
    reloadWith(j, [&](const std::shared_ptr<QueueWindow>& w) {
        w->setRepeatMode(mode);
        loader_->loadWithRepeatMode(w, mode);
    });
}

void QueueManager::procUpdateServerInfo(const Json& j) {
    if (state_ == State::Idle) {
        appNotifyRequestResult(j, kResultIllegalState);
        return;
    }
    const Json& packet = jv::at(j, "packet");
    if (const Json& queueServer = jv::at(packet, "queueServerInfo"); !queueServer.is_null()) {
        loader_->setServerInfo(parseServerInfo(queueServer));
        if (reloadOnServerInfo_ && loading_) {
            loader_->load(loading_);
            reloadOnServerInfo_ = false;
        }
    }
    if (const Json& content = jv::at(packet, "contentServerInfo"); !content.is_null()) {
        playbackSetServerInfo(content);
        if (tokenExpired_) {
            const auto& window = active_ ? active_ : loading_;
            if (!tokenExpiredNext_) {
                log::get()->info("updateServerInfo(token expired): playback set media.");
                if (window)
                    setPlaybackMedia(window->getCurrent(), false);
            } else {
                log::get()->info("updateServerInfo(token expired): playback set next media.");
                Report report;
                if (active_)
                    if (auto item = active_->getNext(report))
                        playbackSetNextMediaInfo(*item);
            }
            tokenExpired_ = tokenExpiredNext_ = false;
        }
    }
    appNotifyRequestResult(j, kResultSuccess);
}

void QueueManager::procPurgeQueue() {
    clearQueueWindow();
    setState(State::Idle);
    util::HttpClient::globalCloseAllKeepalive();
}

// --- From playback -------------------------------------------------------------------

void QueueManager::procNotifyMediaEnded(const Json& j) {
    const auto seq = static_cast<std::uint32_t>(jv::number(j, "mediaSeqNo"));
    const bool hasNextMedia = jv::boolean(j, "hasNextMedia");
    if (mediaSeqNo_ < seq) {
        log::get()->warn("media_seq_no:{} received:{}", mediaSeqNo_, seq);
        return;
    }
    switch (state_) {
    case State::Idle:
        log::get()->warn("State=IDLE. NOTREACHED");
        return;
    case State::Loading:
        pending_.push_back({msg::QueueNotifyMediaEnded, j});
        return;
    case State::Loaded: {
        Report report;
        auto item = active_->next(report);
        if (item && !hasNextMedia) {
            ++mediaSeqNo_;
            setPlaybackMedia(*item, true);
        }
        if (report == Report::OkPrefetch)
            startMoving();
        return;
    }
    case State::Moving: {
        Report report;
        auto item = active_->next(report);
        if (item && !hasNextMedia) {
            ++mediaSeqNo_;
            setPlaybackMedia(*item, false);
        } else if (!item && report == Report::Exhausted) {
            pending_.push_back({msg::QueueNotifyMediaEnded, j});
        }
        return;
    }
    }
}

void QueueManager::procRequestNextMedia(const Json& j) {
    const Json& seqField = jv::at(j, "mediaSeqNo");
    if (!seqField.is_number())
        log::get()->error("[ASSERT] setMedia: mediaSeqNo missing or not a number");
    const auto seq = static_cast<std::uint32_t>(jv::number(seqField));
    if (mediaSeqNo_ < seq) {
        log::get()->warn("media_seq_no:{} received:{}", mediaSeqNo_, seq);
        return;
    }
    switch (state_) {
    case State::Idle:
        return;
    case State::Loading:
        pending_.push_back({msg::QueueRequestNextMedia, j});
        return;
    case State::Loaded:
    case State::Moving: {
        Report report;
        if (auto item = active_->getNext(report))
            playbackSetNextMediaInfo(*item);
        else if (state_ == State::Moving && report == Report::Exhausted)
            pending_.push_back({msg::QueueRequestNextMedia, j});
        return;
    }
    }
}

void QueueManager::procNotifyTokenExpired(const Json& j) {
    tokenExpired_ = true;
    tokenExpiredNext_ = jv::boolean(j, "isNextMedia");
    log::get()->info("playback token expired. isNextMedia?:{}", tokenExpiredNext_);
}

// --- From the loader -----------------------------------------------------------------

void QueueManager::procNotifyLoaded(const Json& j) {
    if (!jv::at(j, "queueId").is_string()) {
        log::get()->error("Invalid message.");
        return;
    }
    if (!loader_->isIdle()) {
        log::get()->info("Ignore notifying onLoaded because QueueWindowLoader is still processing.");
        return;
    }
    if (!loading_ || (state_ != State::Loading && state_ != State::Moving)) {
        log::get()->warn("Illegal state: {}", stateName(state_));
        return;
    }
    active_ = loading_;
    loading_.reset();
    if (state_ == State::Loading && !loadRequest_.is_null()) {
        appNotifyRequestResult(loadRequest_, kResultSuccess);
        loadRequest_ = Json();
    }
    setState(State::Loaded);
    handlePendingMessages();
}

void QueueManager::procNotifyLoadError(const Json& j) {
    const int action = jv::integer(j, "recovery_action");
    const int status = jv::integer(j, "status_code");
    const std::string message = jv::string(j, "response_message");
    if (action == static_cast<int>(QueueWindowLoader::RecoveryAction::Reset)) {
        clearQueueWindow();
        setState(State::Idle);
        loadRequest_ = Json();
    } else if (action == static_cast<int>(QueueWindowLoader::RecoveryAction::RefreshToken)) {
        reloadOnServerInfo_ = true;
    }
    appNotifyQueueServerError(action == static_cast<int>(QueueWindowLoader::RecoveryAction::RefreshToken), status,
                              message);
}

void QueueManager::procNotifyQueueItemsChanged(const Json& j) {
    const int reason = jv::integer(j, "reason");
    switch (static_cast<QueueWindowLoader::Reason>(reason)) {
    case QueueWindowLoader::Reason::ShuffleChanged:
        if (loading_) {
            appNotifyQueueItemsChanged(loading_, "SHUFFLE_CHANGED", 0);
            sendMessage(svc::speaker, msg::SendPacket,
                        {{"packet", {{"command", "setShuffle"}, {"shuffle", loading_->isShuffled()}}}});
        }
        break;
    case QueueWindowLoader::Reason::RepeatModeChanged:
        if (loading_) {
            appNotifyQueueItemsChanged(loading_, "REPEAT_MODE_CHANGED", 0);
            sendMessage(svc::speaker, msg::SendPacket,
                        {{"packet", {{"command", "setRepeatMode"},
                                     {"repeatMode", repeatModeToString(loading_->getRepeatMode())}}}});
        }
        break;
    case QueueWindowLoader::Reason::ItemDeleted:
        if (const auto& w = active_ ? active_ : loading_)
            appNotifyQueueItemsChanged(w, "ITEM_DELETED", 0);
        break;
    default:
        log::get()->error("Illegal Reason: {}", reason);
        break;
    }
}

void QueueManager::procRequestStatusNotification(const Json& j) {
    switch (state_) {
    case State::Idle:
        log::get()->info("State=IDLE; notifyQueueChanged not sent.");
        return;  // not forwarded to playback
    case State::Loading:
        appNotifyQueueChanged(loading_, 0);
        break;
    case State::Loaded:
    case State::Moving:
        appNotifyQueueChanged(active_, 0);
        break;
    }
    sendMessage(svc::playback, msg::RequestStatusNotification, j);
}

// --- Pending messages ----------------------------------------------------------------

bool QueueManager::checkReport(Report r, const Json& req, bool notify) {
    int code = kResultSuccess;
    switch (r) {
    case Report::Ok:
    case Report::OkPrefetch: code = kResultSuccess; break;
    case Report::Exhausted: return false;
    case Report::Begin:
    case Report::End: code = kResultNoMoreItems; break;
    case Report::NotLoaded: code = kResultIllegalState; break;
    default:
        log::get()->critical("[ASSERT] checkReport: report {}", static_cast<int>(r));
        code = kResultIllegalReport;
        break;
    }
    if (notify)
        appNotifyRequestResult(req, code);
    return true;
}

void QueueManager::handlePendingMessages() {
    log::get()->debug("pending message start:{}", pending_.size());
    if (!active_) {
        log::get()->critical("[ASSERT] active_ != nullptr");
        return;
    }
    std::shared_ptr<QueueItem> play;
    std::shared_ptr<QueueItem> nextInfo;
    while (!pending_.empty()) {
        const PendingMessage& m = pending_.front();
        Report report = Report::Ok;
        bool notify = false;
        switch (m.id) {
        case msg::QueueNext:
        case msg::QueuePrevious:
            if (auto item = m.id == msg::QueueNext ? active_->next(report) : active_->previous(report)) {
                play = item;
                ++mediaSeqNo_;
                nextInfo.reset();
            }
            notify = true;
            break;
        case msg::QueueNotifyMediaEnded: {
            const auto seq = static_cast<std::uint32_t>(jv::number(m.data, "mediaSeqNo"));
            if (seq != mediaSeqNo_) {
                log::get()->debug("media_seq_no:{} seq_no:{}", mediaSeqNo_, seq);
                pending_.pop_front();
                continue;
            }
            if (auto item = active_->next(report); item && !jv::boolean(m.data, "hasNextMedia")) {
                play = item;
                ++mediaSeqNo_;
                nextInfo.reset();
            }
            break;
        }
        case msg::QueueRequestNextMedia: {
            const auto seq = static_cast<std::uint32_t>(jv::number(m.data, "mediaSeqNo"));
            if (seq != mediaSeqNo_) {
                pending_.pop_front();
                continue;
            }
            // setNextMedia must carry the current seq.
            if (auto item = active_->getNext(report))
                nextInfo = item;
            break;
        }
        default:
            log::get()->critical("[ASSERT] handlePendingMessages: id 0x{:x}", m.id);
            pending_.pop_front();
            continue;
        }
        if (!checkReport(report, m.data, notify)) {
            log::get()->info("target-item is out of current window.");
            startMoving();
            return;
        }
        pending_.pop_front();
    }
    if (play)
        setPlaybackMedia(*play, false);
    else if (nextInfo)
        playbackSetNextMediaInfo(*nextInfo);
    log::get()->debug("pending message end:{}", pending_.size());
}

void QueueManager::cancelPendingMessages() {
    for (const auto& m : pending_) {
        if (m.id == msg::QueueNext || m.id == msg::QueuePrevious)
            appNotifyRequestResult(m.data, kResultIllegalState);
    }
    pending_.clear();
}

void QueueManager::cancelLoading() {
    if (loader_)
        loader_->reset();
    if (!loadRequest_.is_null()) {
        appNotifyRequestResult(loadRequest_, kResultCancelled);
        loadRequest_ = Json();
    }
    cancelPendingMessages();
    requestItemDelete_ = false;
}

void QueueManager::clearQueueWindow() {
    playbackReset();
    if (loader_)
        loader_->reset();
    if (loading_ && !loadRequest_.is_null())
        appNotifyRequestResult(loadRequest_, kResultSuperseded);
    loadRequest_ = Json();
    cancelPendingMessages();
    active_.reset();
    loading_.reset();
    reloadOnServerInfo_ = tokenExpired_ = tokenExpiredNext_ = requestItemDelete_ = false;
}

// --- Playback -------------------------------------------------------------------------

void QueueManager::resetPlaybackMedia() {
    ++mediaSeqNo_;
    playbackReset();
}

void QueueManager::setPlaybackMedia(const Json& mediaInfo, bool newQueue, int position) {
    if (requestItemDelete_) {
        const auto& w = active_ ? active_ : loading_;
        if (w)
            loader_->deleteItem(w->getQueueId(), jv::string(mediaInfo, "itemId"));
        requestItemDelete_ = false;
    }
    playbackSetMedia(mediaInfo, newQueue, position);
}

void QueueManager::setPlaybackMedia(const QueueItem& item, bool newQueue) {
    setPlaybackMedia(item.makeMediaInfo(), newQueue, 0);
}

void QueueManager::playbackPlay() {
    sendMessage(svc::playback, msg::PlaybackPlay, {{"packet", {{"command", "play"}}}});
}

void QueueManager::playbackStop() {
    sendMessage(svc::playback, msg::PlaybackStop, Json());
}

void QueueManager::playbackReset() {
    sendMessage(svc::playback, msg::PlaybackReset, Json());
}

void QueueManager::playbackSetServerInfo(const Json& info) {
    sendMessage(svc::playback, msg::PlaybackSetServerInfo, info);
}

void QueueManager::playbackSetMedia(const Json& mediaInfo, bool newQueue, int position) {
    sendMessage(svc::playback, msg::PlaybackSetMedia,
                {{"newQueue", newQueue}, {"mediaSeqNo", mediaSeqNo_}, {"mediaInfo", mediaInfo}, {"position", position}});
}

void QueueManager::playbackSetNextMediaInfo(const QueueItem& item) {
    sendMessage(svc::playback, msg::PlaybackSetNextMedia,
                {{"mediaSeqNo", mediaSeqNo_}, {"mediaInfo", item.makeMediaInfo()}});
}

// --- App -------------------------------------------------------------------------------

void QueueManager::appNotifyRequestResult(const Json& req, int code) {
    if (req.is_null())
        return;
    const Json& socketId = jv::at(req, "socketId");
    const bool fromController = jv::number(socketId) == 0;
    sendMessage(fromController ? svc::speaker : svc::clientcomm, msg::SendPacket,
                {{"socketId", socketId.is_null() ? Json(0) : socketId},
                 {"packet", {{"command", "notifyRequestResult"},
                             {"requestId", jv::integer(jv::at(req, "packet"), "requestId")},
                             {"resultCode", code}}}});
}

void QueueManager::appRequestQueueItems(const std::shared_ptr<QueueWindow>& w) {
    sendMessage(svc::clientcomm, msg::SendPacket,
                {{"socketId", loadRequestSocketId_.load()},
                 {"packet", {{"command", "requestQueueItems"},
                             {"queueRequestId", w->getId()},
                             {"queueId", w->getQueueId()},
                             {"itemId", w->getCurrent().getItemId()},
                             {"beforeSize", w->getMaxBeforeSize()},
                             {"afterSize", w->getMaxAfterSize()}}}});
}

void QueueManager::appNotifyQueueServerError(bool toRequester, int subCode, const std::string& description) {
    Json packet{{"command", "notifyQueueServerError"}};
    if (subCode == 0) {
        packet["errorCode"] = 1011;
    } else {
        packet["errorCode"] = 1012;
        packet["details"] = {{"subCode", subCode}, {"description", description}};
    }
    Json body{{"packet", packet}};
    if (toRequester)
        body["socketId"] = loadRequestSocketId_.load();
    sendMessage(svc::clientcomm, msg::SendPacket, body);
}

Json QueueManager::queueInfo(const QueueWindow& w) {
    return {{"queueId", w.getQueueId()},
            {"repeatMode", repeatModeToString(w.getRepeatMode())},
            {"shuffled", w.isShuffled()},
            {"maxBeforeSize", w.getMaxBeforeSize()},
            {"maxAfterSize", w.getMaxAfterSize()}};
}

void QueueManager::appNotifyQueueChanged(const std::shared_ptr<QueueWindow>& w, std::uint32_t excludeSocket) {
    if (!w)
        return;
    Json body{{"packet", {{"command", "notifyQueueChanged"}, {"queueInfo", queueInfo(*w)}}}};
    if (excludeSocket != 0)
        body["exception_socket"] = excludeSocket;
    sendMessage(svc::clientcomm, msg::SendPacket, body);
}

void QueueManager::appNotifyQueueItemsChanged(const std::shared_ptr<QueueWindow>& w, const std::string& reason,
                                              std::uint32_t excludeSocket) {
    if (!w)
        return;
    Json body{{"packet", {{"command", "notifyQueueItemsChanged"}, {"reason", reason}, {"queueInfo", queueInfo(*w)}}}};
    if (excludeSocket != 0)
        body["exception_socket"] = excludeSocket;
    sendMessage(svc::clientcomm, msg::SendPacket, body);
}

QueueWindowLoader::ServerInfo QueueManager::parseServerInfo(const Json& j) {
    QueueWindowLoader::ServerInfo info;
    info.serverUrl = jv::string(j, "serverUrl");
    const Json& auth = jv::at(j, "authInfo");
    info.headerAuth = jv::string(auth, "headerAuth");
    const Json& oauth = jv::at(auth, "oauthServerInfo");
    info.oauthServerUrl = jv::string(oauth, "serverUrl");
    const Json& oauthAuth = jv::at(oauth, "authInfo");
    info.oauthHeaderAuth = jv::string(oauthAuth, "headerAuth");
    const Json& params = jv::at(oauthAuth, "oauthParameters");
    info.oauthAccessToken = jv::string(params, "accessToken");
    info.oauthRefreshToken = jv::string(params, "refreshToken");
    for (const auto& h : jv::at(oauth, "httpHeaderFields"))
        if (h.is_string())
            info.oauthHttpHeaderFields.push_back(h.get<std::string>());
    for (const auto& h : jv::at(j, "httpHeaderFields"))
        if (h.is_string())
            info.httpHeaderFields.push_back(h.get<std::string>());
    return info;
}

}  // namespace tcng::queue
