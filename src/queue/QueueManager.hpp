#pragma once
// The "queue" service: owns the play queue (as windows from the
// queue server or the app), drives playback with setMedia/setNextMedia and the media
// sequence number, and handles next/previous/select/shuffle/repeat/refresh.

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "core/MessageCore.hpp"
#include "queue/QueueWindow.hpp"
#include "queue/QueueWindowLoader.hpp"

namespace tcng::queue {

class QueueManager : public core::Service {
public:
    enum class State : int { Idle = 0, Loading = 1, Loaded = 2, Moving = 3 };

    QueueManager();
    ~QueueManager() override;

    void start() override;
    void finish() override;

    State getState() const { return state_; }

protected:
    void handleMessage(std::uint32_t id, const core::Json& payload) override;

private:
    class WindowDelegate;
    class LoaderDelegate;
    using Report = QueueWindow::Report;

    struct PendingMessage {
        std::uint32_t id;
        core::Json data;
    };

    void setState(State state);

    // Handlers
    void loadQueueWindow(const core::Json& j);
    void procRespondQueueItems(const core::Json& j);
    void procNext(const core::Json& j);
    void procPrevious(const core::Json& j);
    void procSelectQueueItem(const core::Json& j);
    void procRefreshQueue(const core::Json& j);
    void procSetShuffle(const core::Json& j);
    void procSetRepeatMode(const core::Json& j);
    void procUpdateServerInfo(const core::Json& j);
    void procPurgeQueue();
    void procNotifyMediaEnded(const core::Json& j);
    void procRequestNextMedia(const core::Json& j);
    void procNotifyTokenExpired(const core::Json& j);
    void procNotifyLoaded(const core::Json& j);
    void procNotifyLoadError(const core::Json& j);
    void procNotifyQueueItemsChanged(const core::Json& j);
    void procRequestStatusNotification(const core::Json& j);

    // Helpers
    void navigate(const core::Json& j, bool forward);
    void startMoving();
    void reloadWith(const core::Json& j, const std::function<void(const std::shared_ptr<QueueWindow>&)>& load);
    bool checkReport(Report r, const core::Json& req, bool notify);
    void handlePendingMessages();
    void cancelPendingMessages();
    void cancelLoading();
    void clearQueueWindow();
    void resetPlaybackMedia();
    void setPlaybackMedia(const core::Json& mediaInfo, bool newQueue, int position);
    void setPlaybackMedia(const QueueItem& item, bool newQueue);
    void saveLoadRequest(const core::Json& j);
    static QueueWindowLoader::ServerInfo parseServerInfo(const core::Json& j);

    // Outbound
    void playbackPlay();
    void playbackStop();
    void playbackReset();
    void playbackSetServerInfo(const core::Json& info);
    void playbackSetMedia(const core::Json& mediaInfo, bool newQueue, int position);
    void playbackSetNextMediaInfo(const QueueItem& item);
    void appNotifyRequestResult(const core::Json& req, int code);
    void appRequestQueueItems(const std::shared_ptr<QueueWindow>& window);
    void appNotifyQueueServerError(bool toRequester, int subCode, const std::string& description);
    void appNotifyQueueChanged(const std::shared_ptr<QueueWindow>& window, std::uint32_t excludeSocket);
    void appNotifyQueueItemsChanged(const std::shared_ptr<QueueWindow>& window, const std::string& reason,
                                    std::uint32_t excludeSocket);
    static core::Json queueInfo(const QueueWindow& window);

    static constexpr unsigned kMaxWindowSize = 20;

    std::shared_ptr<WindowDelegate> windowDelegate_;
    std::shared_ptr<LoaderDelegate> loaderDelegate_;
    std::shared_ptr<QueueWindowLoader> loader_;

    // Queue thread only.
    State state_ = State::Idle;
    std::shared_ptr<QueueWindow> active_;
    std::shared_ptr<QueueWindow> loading_;
    std::uint32_t mediaSeqNo_ = 0;
    bool reloadOnServerInfo_ = false;
    bool tokenExpired_ = false;
    bool tokenExpiredNext_ = false;
    core::Json loadRequest_;
    std::deque<PendingMessage> pending_;
    bool requestItemDelete_ = false;

    // Read by the loader thread (requestQueueItems goes to the loading socket).
    std::atomic<std::uint32_t> loadRequestSocketId_{0};
};

}  // namespace tcng::queue
