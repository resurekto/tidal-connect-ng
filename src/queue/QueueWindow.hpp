#pragma once
// QueueWindow: a slice of the queue around the current item (at most 20 before
// and 20 after). Navigation reports when the edge is near (prefetch) or reached; a new
// window centred on the new item is then loaded.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "queue/QueueItem.hpp"

namespace tcng::queue {

enum class RepeatMode : int { Off = 0, All = 1, One = 2 };

RepeatMode repeatModeFromString(const std::string& s);  // "ALL", "ONE", else OFF
const char* repeatModeToString(RepeatMode m);           // "OFF", "ALL", "ONE"

class QueueWindow {
public:
    enum class State : int { Unloaded = 0, ItemsChanged = 1, Loaded = 2 };
    enum class Report : int {
        Ok = 0,
        OkPrefetch = 1,  // the edge is near: load the next window
        Exhausted = 2,   // target outside this window: load and retry
        Begin = 3,
        End = 4,
        NotFound = 5,
        NotLoaded = 6,
    };

    class Delegate {
    public:
        virtual ~Delegate() = default;
        virtual void onError() = 0;
        virtual void onRequestItemDelete() = 0;
    };

    QueueWindow(std::string queueId, RepeatMode repeatMode, bool shuffled, unsigned maxBeforeSize,
                unsigned maxAfterSize, QueueItem current);

    void setDelegate(const std::shared_ptr<Delegate>& delegate) { delegate_ = delegate; }
    std::shared_ptr<QueueWindow> getNewWindow() const;
    // Same, centred on another item.
    std::shared_ptr<QueueWindow> getNewWindow(const QueueItem& current) const;
    bool isNewQueue() const { return moveCount_ == 0; }

    std::shared_ptr<QueueItem> previous(Report& report);
    std::shared_ptr<QueueItem> next(Report& report);
    std::shared_ptr<QueueItem> getNext(Report& report) const;
    std::shared_ptr<QueueItem> select(const std::string& itemId, Report& report);
    void storeQueueItems(const std::vector<QueueItem>& items, bool hasBegin, bool hasEnd);

    std::uint32_t getId() const { return id_; }
    const std::string& getQueueId() const { return queueId_; }
    RepeatMode getRepeatMode() const { return repeatMode_; }
    void setRepeatMode(RepeatMode m) { repeatMode_ = m; }
    bool isShuffled() const { return shuffled_; }
    void setShuffled(bool s) { shuffled_ = s; }
    unsigned getMaxBeforeSize() const { return maxBeforeSize_; }
    unsigned getMaxAfterSize() const { return maxAfterSize_; }
    State getState() const { return state_; }
    void setState(State s) { state_ = s; }
    const QueueItem& getCurrent() const { return current_; }
    const std::vector<QueueItem>& getItems() const { return items_; }

private:
    void requestItemDelete();

    static constexpr unsigned kThreshold = 5;

    std::uint32_t id_;
    std::string queueId_;
    RepeatMode repeatMode_;
    bool shuffled_;
    unsigned maxBeforeSize_;
    unsigned maxAfterSize_;
    State state_ = State::Unloaded;
    std::vector<QueueItem> items_;
    QueueItem current_;
    unsigned beforeSize_ = 0;
    unsigned afterSize_ = 0;
    bool hasBegin_ = false;
    bool hasEnd_ = false;
    std::weak_ptr<Delegate> delegate_;
    unsigned moveCount_ = 0;
};

}  // namespace tcng::queue
