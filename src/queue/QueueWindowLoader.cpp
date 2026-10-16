#include "queue/QueueWindowLoader.hpp"

#include <nlohmann/json.hpp>

#include "util/JsonUtil.hpp"
#include "util/Log.hpp"

namespace tcng::queue {

using Json = nlohmann::json;
using util::HttpMethod;
using util::HttpRequest;
using util::HttpResponse;

QueueWindowLoader::QueueWindowLoader(std::weak_ptr<Delegate> delegate)
    : delegate_(std::move(delegate)), http_(std::make_shared<util::AsyncHttpClient>(1)) {
    http_->start();
}

QueueWindowLoader::~QueueWindowLoader() {
    http_->stop();
}

std::shared_ptr<QueueWindowLoader::Delegate> QueueWindowLoader::delegate() const {
    auto d = delegate_.lock();
    if (!d)
        log::get()->critical("[ASSERT] delegate_.lock() != nullptr");
    return d;
}

void QueueWindowLoader::setServerInfo(const ServerInfo& info) {
    std::lock_guard lock(mutex_);
    server_ = info;
    isCloud_ = true;
    log::get()->trace("server_url : {}", server_.serverUrl);
}

void QueueWindowLoader::enableServerVerification(bool enable) {
    http_->enableServerVerification(enable);
}

bool QueueWindowLoader::isIdle() const {
    std::lock_guard lock(mutex_);
    return state_ == State::Idle;
}

void QueueWindowLoader::backToIdle() {
    taskId_ = 0;
    loading_.reset();
    state_ = State::Idle;
}

void QueueWindowLoader::cancelLoading() {
    if (taskId_ != 0 && state_ == State::Loading)
        http_->cancel(taskId_);
    backToIdle();
}

void QueueWindowLoader::cancel() {
    std::lock_guard lock(mutex_);
    cancelLoading();
}

void QueueWindowLoader::reset() {
    std::lock_guard lock(mutex_);
    cancelLoading();
    server_ = ServerInfo{};
    isCloud_ = false;
}

std::uint32_t QueueWindowLoader::send(const util::HttpRequest& request) {
    const auto id = http_->request(request, shared_from_this());
    if (id == 0)
        log::get()->critical("[ASSERT] send: request id 0");
    return id;
}

void QueueWindowLoader::load(const std::shared_ptr<QueueWindow>& window) {
    std::shared_ptr<QueueWindow> askApp;
    {
        std::lock_guard lock(mutex_);
        cancelLoading();
        loading_ = window;
        state_ = State::Loading;
        if (isCloud_)
            taskId_ = send(buildGetWindowRequest(*loading_));
        else
            askApp = loading_;
    }
    if (askApp)
        if (auto d = delegate())
            d->onRequestQueueItems(askApp);
}

void QueueWindowLoader::loadWithRepeatMode(const std::shared_ptr<QueueWindow>& window, RepeatMode mode) {
    std::shared_ptr<QueueWindow> askApp;
    {
        std::lock_guard lock(mutex_);
        cancelLoading();
        loading_ = window;
        if (isCloud_) {
            state_ = State::SetRepeatMode;
            taskId_ = send(buildPutRepeatModeRequest(*loading_, mode));
        } else {
            state_ = State::Loading;
            askApp = loading_;
        }
    }
    if (askApp)
        if (auto d = delegate())
            d->onRequestQueueItems(askApp);
}

void QueueWindowLoader::loadWithShuffle(const std::shared_ptr<QueueWindow>& window, bool shuffle) {
    std::shared_ptr<QueueWindow> askApp;
    {
        std::lock_guard lock(mutex_);
        cancelLoading();
        loading_ = window;
        if (isCloud_) {
            state_ = State::SetShuffle;
            taskId_ = send(buildPutShuffleRequest(*loading_, shuffle));
        } else {
            state_ = State::Loading;
            askApp = loading_;
        }
    }
    if (askApp)
        if (auto d = delegate())
            d->onRequestQueueItems(askApp);
}

void QueueWindowLoader::respondQueueItems(const std::vector<QueueItem>& items, bool hasBegin, bool hasEnd) {
    std::shared_ptr<QueueWindow> loaded;
    {
        std::lock_guard lock(mutex_);
        if (isCloud_)
            log::get()->critical("[ASSERT] !isCloud_");
        if (state_ != State::Loading || !loading_) {
            log::get()->critical("[ASSERT] state_ == LOADING");
            return;
        }
        loading_->storeQueueItems(items, hasBegin, hasEnd);
        loaded = loading_;
        backToIdle();
    }
    if (auto d = delegate())
        d->onLoaded(loaded);
}

void QueueWindowLoader::deleteItem(const std::string& queueId, const std::string& itemId) {
    std::lock_guard lock(mutex_);
    if (!isCloud_)
        return;
    const auto id = send(buildDeleteRequest(queueId, itemId));
    deletes_[id] = DeleteTrackInfo{queueId, itemId, 0};
}

bool QueueWindowLoader::activeTrackHandling(std::uint32_t id, const util::HttpResponse& response) {
    bool notifyDeleted = false;
    {
        std::lock_guard lock(mutex_);
        auto it = deletes_.find(id);
        if (it == deletes_.end())
            return false;
        DeleteTrackInfo info = it->second;
        deletes_.erase(it);
        if (response.status == 200 || response.status == 204) {
            log::get()->debug("delete Item success.");
            notifyDeleted = deletes_.empty();
        } else {
            log::get()->warn("delete Item error, status code:{}", response.status);
            if (info.retry < kMaxDeleteRetry) {
                ++info.retry;
                log::get()->warn("Active Track delete Error. queueId:{} itemId:{} retry:{}", info.queueId,
                                 info.itemId, info.retry);
                const auto retryId = send(buildDeleteRequest(info.queueId, info.itemId));
                deletes_[retryId] = info;
            }
        }
    }
    if (notifyDeleted)
        if (auto d = delegate())
            d->onQueueItemsChanged(Reason::ItemDeleted);
    return true;
}

void QueueWindowLoader::onReceived(std::uint32_t id, const util::HttpResponse& response) {
    if (activeTrackHandling(id, response))
        return;

    enum class Then { Nothing, Loaded, ItemsChanged, LoadError } then = Then::Nothing;
    Reason reason = Reason::RepeatModeChanged;
    RecoveryAction action = RecoveryAction::Reset;
    std::shared_ptr<QueueWindow> window;
    {
        std::lock_guard lock(mutex_);
        if (id != taskId_) {
            log::get()->debug("request id is old. now request[{}], response[{}]", taskId_, id);
            return;
        }
        log::get()->debug("recv data={}", response.bodyString());
        const int status = response.status;
        if (status != 200 && status != 204) {
            if (status == 400 || status == 404 || status == 500) {
                then = Then::LoadError;
                action = RecoveryAction::Reset;
            } else if (status == 401) {
                then = Then::LoadError;
                action = RecoveryAction::RefreshToken;
            } else {
                log::get()->warn("status code:{}", status);
            }
            backToIdle();
        } else {
            switch (state_) {
            case State::SetRepeatMode:
            case State::SetShuffle:
                reason = state_ == State::SetRepeatMode ? Reason::RepeatModeChanged : Reason::ShuffleChanged;
                loading_->setState(QueueWindow::State::ItemsChanged);
                state_ = State::Loading;
                taskId_ = send(buildGetWindowRequest(*loading_));
                then = Then::ItemsChanged;
                break;
            case State::Loading: {
                std::vector<QueueItem> items;
                bool hasBegin = false, hasEnd = false;
                if (parseGetWindowResponse(response, items, hasBegin, hasEnd)) {
                    loading_->storeQueueItems(items, hasBegin, hasEnd);
                    window = loading_;
                    backToIdle();
                    then = Then::Loaded;
                } else {
                    // Report it as a load error.
                    log::get()->error("failed to parse response data.");
                    backToIdle();
                    then = Then::LoadError;
                    action = RecoveryAction::Reset;
                }
                break;
            }
            case State::Idle:
                log::get()->warn("Illegal state : {}", static_cast<int>(state_));
                break;
            }
        }
    }
    auto d = delegate();
    if (!d)
        return;
    switch (then) {
    case Then::Loaded: d->onLoaded(window); break;
    case Then::ItemsChanged: d->onQueueItemsChanged(reason); break;
    case Then::LoadError: d->onLoadError(action, response); break;
    case Then::Nothing: break;
    }
}

// --- Requests -----------------------------------------------------------------------

std::vector<std::string> QueueWindowLoader::headers(bool json) const {
    std::vector<std::string> h;
    if (!server_.headerAuth.empty())
        h.push_back("Authorization: " + server_.headerAuth);
    else if (!server_.oauthAccessToken.empty())
        h.push_back("Authorization: Bearer " + server_.oauthAccessToken);
    for (const auto& field : server_.httpHeaderFields)
        h.push_back(field);
    if (json)
        h.push_back("Content-Type: application/json");
    return h;
}

util::HttpRequest QueueWindowLoader::buildGetWindowRequest(const QueueWindow& w) const {
    util::HttpRequest r;
    r.method = util::HttpMethod::Get;
    r.url = server_.serverUrl + "/" + w.getQueueId() + "/window";
    r.query = {{"itemid", w.getCurrent().getItemId()},
               {"beforesize", std::to_string(w.getMaxBeforeSize())},
               {"aftersize", std::to_string(w.getMaxAfterSize())}};
    r.headers = headers(false);
    return r;
}

util::HttpRequest QueueWindowLoader::buildPutRepeatModeRequest(const QueueWindow& w, RepeatMode mode) const {
    util::HttpRequest r;
    r.method = util::HttpMethod::Put;
    r.url = server_.serverUrl + "/" + w.getQueueId() + "/repeatmode";
    const char* value = mode == RepeatMode::One ? "one" : mode == RepeatMode::All ? "all" : "off";
    r.body = Json{{"repeat_mode", value}}.dump();
    r.headers = headers(true);
    return r;
}

util::HttpRequest QueueWindowLoader::buildPutShuffleRequest(const QueueWindow& w, bool shuffle) const {
    util::HttpRequest r;
    r.method = util::HttpMethod::Put;
    r.url = server_.serverUrl + "/" + w.getQueueId() + "/shuffle";
    r.body = Json{{"shuffle", shuffle}}.dump();
    r.headers = headers(true);
    return r;
}

util::HttpRequest QueueWindowLoader::buildDeleteRequest(const std::string& queueId, const std::string& itemId) const {
    util::HttpRequest r;
    r.method = util::HttpMethod::Delete;
    r.url = server_.serverUrl + "/" + queueId + "/items/before";
    r.query = {{"itemid", itemId}, {"onlyactives", "true"}};
    r.headers = headers(true);
    return r;
}

bool QueueWindowLoader::parseGetWindowResponse(const util::HttpResponse& response, std::vector<QueueItem>& items,
                                               bool& hasBegin, bool& hasEnd) {
    Json j = Json::parse(response.bodyString(), nullptr, false);
    if (j.is_discarded()) {
        log::get()->error("parse error : {}", response.bodyString());
        return false;
    }
    const Json& arr = jv::at(j, "items");
    if (!arr.is_array()) {
        log::get()->warn("no item");
        return false;
    }
    hasBegin = jv::boolean(j, "includes_head");
    hasEnd = jv::boolean(j, "includes_tail");
    items.clear();
    for (const auto& elem : arr)
        items.emplace_back(elem, false);
    return true;
}

}  // namespace tcng::queue
