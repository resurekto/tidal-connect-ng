#pragma once
// QueueWindowLoader: fills a QueueWindow, either from the TIDAL queue server
// (cloud: GET …/window, PUT …/repeatmode|shuffle, DELETE …/items/before) or by asking
// the app (local: requestQueueItems / respondQueueItems).

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "util/AsyncHttpClient.hpp"
#include "queue/QueueWindow.hpp"

namespace tcng::queue {

class QueueWindowLoader : public util::AsyncHttpClientDelegate,
                          public std::enable_shared_from_this<QueueWindowLoader> {
public:
    struct ServerInfo {
        std::string serverUrl;
        std::string headerAuth;
        std::string oauthServerUrl;
        std::string oauthHeaderAuth;
        std::string oauthAccessToken;
        std::string oauthRefreshToken;
        std::vector<std::string> oauthHttpHeaderFields;
        std::vector<std::string> httpHeaderFields;  // appended to every request
    };

    enum class State : int { Idle = 0, Loading = 1, SetRepeatMode = 2, SetShuffle = 3 };
    enum class RecoveryAction : int { Reset = 0, RefreshToken = 1 };
    enum class Reason : int { RepeatModeChanged = 0, ShuffleChanged = 1, ItemDeleted = 2 };

    class Delegate {
    public:
        virtual ~Delegate() = default;
        virtual void onRequestQueueItems(const std::shared_ptr<QueueWindow>& window) = 0;
        virtual void onLoaded(const std::shared_ptr<QueueWindow>& window) = 0;
        virtual void onQueueItemsChanged(Reason reason) = 0;
        virtual void onLoadError(RecoveryAction action, const util::HttpResponse& response) = 0;
    };

    explicit QueueWindowLoader(std::weak_ptr<Delegate> delegate);
    ~QueueWindowLoader() override;

    void setServerInfo(const ServerInfo& info);
    void enableServerVerification(bool enable);
    void load(const std::shared_ptr<QueueWindow>& window);
    void loadWithRepeatMode(const std::shared_ptr<QueueWindow>& window, RepeatMode mode);
    void loadWithShuffle(const std::shared_ptr<QueueWindow>& window, bool shuffle);
    void respondQueueItems(const std::vector<QueueItem>& items, bool hasBegin, bool hasEnd);
    void deleteItem(const std::string& queueId, const std::string& itemId);
    void cancel();
    void reset();
    bool isIdle() const;

    void onReceived(std::uint32_t id, const util::HttpResponse& response) override;

    // Request builders (exposed for tests).
    util::HttpRequest buildGetWindowRequest(const QueueWindow& window) const;
    util::HttpRequest buildPutRepeatModeRequest(const QueueWindow& window, RepeatMode mode) const;
    util::HttpRequest buildPutShuffleRequest(const QueueWindow& window, bool shuffle) const;
    util::HttpRequest buildDeleteRequest(const std::string& queueId, const std::string& itemId) const;
    static bool parseGetWindowResponse(const util::HttpResponse& response, std::vector<QueueItem>& items,
                                       bool& hasBegin, bool& hasEnd);

private:
    struct DeleteTrackInfo {
        std::string queueId;
        std::string itemId;
        unsigned retry = 0;
    };

    std::vector<std::string> headers(bool json) const;
    void cancelLoading();  // mutex held
    void backToIdle();     // mutex held
    std::uint32_t send(const util::HttpRequest& request);
    bool activeTrackHandling(std::uint32_t id, const util::HttpResponse& response);
    std::shared_ptr<Delegate> delegate() const;

    static constexpr unsigned kMaxDeleteRetry = 3;

    mutable std::mutex mutex_;
    std::weak_ptr<Delegate> delegate_;
    std::shared_ptr<util::AsyncHttpClient> http_;
    State state_ = State::Idle;
    bool isCloud_ = false;
    std::uint32_t taskId_ = 0;
    std::map<std::uint32_t, DeleteTrackInfo> deletes_;
    ServerInfo server_;
    std::shared_ptr<QueueWindow> loading_;
};

}  // namespace tcng::queue
