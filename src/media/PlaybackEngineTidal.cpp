#include "media/PlaybackEngineTidal.hpp"

#include <nlohmann/json.hpp>

#include "util/Log.hpp"
#include "media/TidalUrlFetchTask.hpp"

namespace tcng::media {

PlaybackEngineTidal::PlaybackEngineTidal(std::string clientId, std::shared_ptr<const ITrackFetchTaskFactory> fetchTasks)
    : clientId_(std::move(clientId)),
      fetchTasks_(fetchTasks ? std::move(fetchTasks) : std::make_shared<TidalUrlFetchTaskFactory>()),
      pool_(std::make_shared<util::ThreadPool>(1)) {}

PlaybackEngineTidal::~PlaybackEngineTidal() {
    pool_->stop();
}

int PlaybackEngineTidal::connect(const std::shared_ptr<IMediaPlayer>& player, const std::shared_ptr<Delegate>& delegate) {
    PlaybackEngineGeneric::connect(player, delegate);
    auto self = std::static_pointer_cast<PlaybackEngineTidal>(shared_from_this());
    pool_->setDelegate(std::weak_ptr<util::ThreadPool::Delegate>(self));
    pool_->start();
    return kSuccess;
}

int PlaybackEngineTidal::disconnect() {
    {
        std::lock_guard lock(mutex_);
        cancelPending();
    }
    pool_->stop();  // outside the engine mutex (onCompleted takes it)
    return PlaybackEngineGeneric::disconnect();
}

void PlaybackEngineTidal::cancelPending() {
    if (pendingTaskId_ != 0) {
        pool_->cancelTask(pendingTaskId_);
        pendingTaskId_ = 0;
    }
    pendingIsNext_ = false;
}

int PlaybackEngineTidal::reset() {
    std::lock_guard lock(mutex_);
    cancelPending();
    if (!player_)
        return 1;
    const int rc = player_->stop();
    notifyStopped(lockDelegate());
    return rc;
}

int PlaybackEngineTidal::stop() {
    std::lock_guard lock(mutex_);
    seq_ = 0;
    cancelPending();
    if (!player_)
        return 1;
    const int rc = player_->stop();
    notifyStopped(lockDelegate());
    return rc;
}

int PlaybackEngineTidal::clearToken() {
    std::lock_guard lock(mutex_);
    switchedToken_.clear();
    return kSuccess;
}

int PlaybackEngineTidal::enableServerVerification(bool enable) {
    if (!enable)
        log::get()->warn("enableServerVerification(false) ignored: TLS verification is always on");
    return kSuccess;
}

int PlaybackEngineTidal::prepare(std::uint32_t seq, const ServerInfo& si, const std::string& mediaId, unsigned type) {
    std::lock_guard lock(mutex_);
    seq_ = seq;
    sendTidalHttpRequest(si, mediaId, false, type);
    return kSuccess;
}

int PlaybackEngineTidal::prepareNext(std::uint32_t seq, const ServerInfo& si, const std::string& mediaId, unsigned type) {
    std::lock_guard lock(mutex_);
    if (seq != seq_) {
        log::get()->debug("old media_seqno: now={}, in={}", seq_.load(), seq);
        return 1;
    }
    sendTidalHttpRequest(si, mediaId, true, type);
    return kSuccess;
}

// sendTidalHttpRequest. Called with mutex_ held.
void PlaybackEngineTidal::sendTidalHttpRequest(const ServerInfo& si, const std::string& mediaId, bool isNext,
                                               unsigned type) {
    if (pendingTaskId_ != 0) {
        pool_->cancelTask(pendingTaskId_);
        pendingIsNext_ = false;
    }
    auto task = fetchTasks_->create(TrackFetchRequest{si, clientId_, mediaId, switchedToken_, type});
    pendingTaskId_ = pool_->postTask(task);
    if (pendingTaskId_ == 0)
        log::get()->error("[ASSERT] {}", "pendingTaskId_ != 0");
    log::get()->debug("url fetch start: {}", pendingTaskId_);
    pendingIsNext_ = isNext;
}

PlaybackEngineGeneric::RecoveryAction PlaybackEngineTidal::classifyStatus(const util::HttpResponse& response, bool* clearToken) {
    if (clearToken)
        *clearToken = false;
    const auto body = nlohmann::json::parse(response.bodyString(), nullptr, false);
    long long subStatus = 0;
    bool found = false;
    if (body.is_object()) {
        for (const char* key : {"sub_status", "subStatus"}) {
            auto it = body.find(key);
            if (it != body.end() && it->is_number()) {
                subStatus = it->get<long long>();
                found = true;
                break;
            }
        }
    }
    if (!found) {
        log::get()->debug("no sub status");
        return RecoveryAction::Default;
    }
    if (response.status >= 400 && response.status <= 499) {
        if (subStatus == 4005 || subStatus == 4007)
            return RecoveryAction::SkipTrack;
        if (subStatus >= 11001 && subStatus <= 11003 && clearToken)
            *clearToken = true;
    }
    return RecoveryAction::Default;
}

PlaybackEngineGeneric::RecoveryAction PlaybackEngineTidal::checkStatusCode(const util::HttpResponse& response) {
    bool clear = false;
    const auto action = classifyStatus(response, &clear);
    if (clear)
        switchedToken_.clear();  // clearToken(); mutex_ already held by onCompleted
    return action;
}

// onCompleted, on the pool thread.
void PlaybackEngineTidal::onCompleted(const std::shared_ptr<util::ThreadPool::ITask>& task) {
    auto fetch = std::dynamic_pointer_cast<ITrackFetchTask>(task);
    if (!fetch)
        return;
    std::lock_guard lock(mutex_);
    if (fetch->getId() != pendingTaskId_) {
        log::get()->debug("url fetch discard: {}", fetch->getId());
        return;
    }
    fetch->getResult(result_, switchedToken_);
    const util::HttpResponse& response = fetch->response();
    const bool isNext = pendingIsNext_;
    const std::uint32_t seq = seq_;
    auto delegate = lockDelegate();
    if (response.status == 200 && player_) {
        if (isNext) {
            player_->prepareNext(result_);
            if (delegate)
                delegate->onPreparedNext(seq, result_);
        } else {
            player_->prepare(result_);
            if (delegate)
                delegate->onPrepared(seq, result_);
        }
    } else {
        const auto action = checkStatusCode(response);
        if (delegate) {
            if (isNext)
                delegate->onPrepareNextError(seq, action, response);
            else
                delegate->onPrepareError(seq, action, response);
        }
    }
    pendingTaskId_ = 0;
    pendingIsNext_ = false;
}

}  // namespace tcng::media
