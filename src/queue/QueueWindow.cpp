#include "queue/QueueWindow.hpp"

#include <atomic>

#include "util/Log.hpp"

namespace tcng::queue {

RepeatMode repeatModeFromString(const std::string& s) {
    if (s == "ALL")
        return RepeatMode::All;
    if (s == "ONE")
        return RepeatMode::One;
    return RepeatMode::Off;
}

const char* repeatModeToString(RepeatMode m) {
    switch (m) {
    case RepeatMode::All: return "ALL";
    case RepeatMode::One: return "ONE";
    default: return "OFF";
    }
}

namespace {
std::uint32_t generateId() {
    static std::atomic<std::uint32_t> counter{0};
    return counter++;
}
}  // namespace

QueueWindow::QueueWindow(std::string queueId, RepeatMode repeatMode, bool shuffled, unsigned maxBeforeSize,
                         unsigned maxAfterSize, QueueItem current)
    : id_(generateId()),
      queueId_(std::move(queueId)),
      repeatMode_(repeatMode),
      shuffled_(shuffled),
      maxBeforeSize_(maxBeforeSize),
      maxAfterSize_(maxAfterSize),
      current_(std::move(current)) {}

std::shared_ptr<QueueWindow> QueueWindow::getNewWindow() const {
    return getNewWindow(current_);
}

std::shared_ptr<QueueWindow> QueueWindow::getNewWindow(const QueueItem& current) const {
    auto w = std::make_shared<QueueWindow>(queueId_, repeatMode_, shuffled_, maxBeforeSize_, maxAfterSize_, current);
    w->delegate_ = delegate_;
    w->moveCount_ = moveCount_ + 1;
    log::get()->debug("[{}] <move> mode:{}/{}, count:{}", w->id_, repeatModeToString(repeatMode_), shuffled_,
                      w->moveCount_);
    return w;
}

void QueueWindow::requestItemDelete() {
    if (auto d = delegate_.lock())
        d->onRequestItemDelete();
}

std::shared_ptr<QueueItem> QueueWindow::previous(Report& report) {
    report = Report::NotLoaded;
    std::shared_ptr<QueueItem> ret;
    if (state_ != State::Unloaded) {
        report = Report::Ok;
        if (beforeSize_ == 0) {
            report = hasBegin_ ? Report::Begin : Report::Exhausted;
        } else {
            --beforeSize_;
            ++afterSize_;
            current_ = items_[beforeSize_];
            if (beforeSize_ < kThreshold && !hasBegin_)
                report = Report::OkPrefetch;
            ret = std::make_shared<QueueItem>(current_);
        }
    }
    log::get()->debug("[{}] report:{}, item:{}", id_, static_cast<int>(report), ret ? ret->getInfo() : "null");
    return ret;
}

std::shared_ptr<QueueItem> QueueWindow::getNext(Report& report) const {
    report = Report::NotLoaded;
    std::shared_ptr<QueueItem> ret;
    if (state_ != State::Unloaded) {
        report = Report::Ok;
        if (afterSize_ == 0)
            report = hasEnd_ ? Report::End : Report::Exhausted;
        else
            ret = std::make_shared<QueueItem>(items_[beforeSize_ + 1]);
    }
    log::get()->debug("[{}] report:{}, item:{}", id_, static_cast<int>(report), ret ? ret->getInfo() : "null");
    return ret;
}

std::shared_ptr<QueueItem> QueueWindow::next(Report& report) {
    report = Report::NotLoaded;
    std::shared_ptr<QueueItem> ret;
    if (state_ != State::Unloaded) {
        report = Report::Ok;
        const bool dropCurrent = current_.isActive() && repeatMode_ != RepeatMode::One && beforeSize_ < items_.size();
        if (dropCurrent) {
            // An "active" (one-shot) item is removed once played; the following item then
            // sits at the same index.
            log::get()->debug("[{}] Active Track Execute", id_);
            requestItemDelete();
            items_.erase(items_.begin() + beforeSize_);
        }
        if (afterSize_ == 0) {
            report = hasEnd_ ? Report::End : Report::Exhausted;
        } else {
            if (!dropCurrent)
                ++beforeSize_;
            --afterSize_;
            current_ = items_[beforeSize_];
            if (afterSize_ < kThreshold && !hasEnd_)
                report = Report::OkPrefetch;
            ret = std::make_shared<QueueItem>(current_);
        }
    }
    log::get()->debug("[{}] report:{}, item:{}", id_, static_cast<int>(report), ret ? ret->getInfo() : "null");
    return ret;
}

std::shared_ptr<QueueItem> QueueWindow::select(const std::string& itemId, Report& report) {
    report = Report::NotLoaded;
    if (state_ == State::Unloaded)
        return nullptr;
    report = Report::Ok;
    std::size_t idx = 0;
    while (idx < items_.size() && items_[idx].getItemId() != itemId)
        ++idx;
    if (idx == items_.size()) {
        report = Report::NotFound;
        current_ = QueueItem(nlohmann::json{{"itemId", itemId}}, true);  // placeholder for the reload
        return nullptr;
    }
    beforeSize_ = static_cast<unsigned>(idx);
    afterSize_ = static_cast<unsigned>(items_.size() - idx - 1);
    current_ = items_[idx];
    bool removed = false;
    for (std::size_t i = idx; i-- > 0;) {
        if (items_[i].isActive()) {
            log::get()->debug("[SELECT]Active Track Execute");
            items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
            --beforeSize_;
            removed = true;
        }
    }
    auto ret = std::make_shared<QueueItem>(current_);
    if ((afterSize_ < kThreshold && !hasEnd_) || (beforeSize_ < kThreshold && !hasBegin_))
        report = Report::OkPrefetch;
    if (removed)
        requestItemDelete();
    return ret;
}

void QueueWindow::storeQueueItems(const std::vector<QueueItem>& items, bool hasBegin, bool hasEnd) {
    if (state_ == State::Loaded) {
        log::get()->critical("[ASSERT] storeQueueItems: window {} already loaded", id_);
        return;
    }
    items_ = items;
    hasBegin_ = hasBegin;
    hasEnd_ = hasEnd;
    log::get()->debug("[{}] list size:{} has_begin:{} has_end:{}", id_, items_.size(), hasBegin_, hasEnd_);

    auto find = [&](auto pred) {
        for (std::size_t i = 0; i < items_.size(); ++i)
            if (pred(items_[i]))
                return static_cast<long>(i);
        return -1L;
    };
    long idx = find([&](const QueueItem& it) { return it.getItemId() == current_.getItemId(); });
    if (idx < 0 && !current_.getMediaId().empty()) {
        idx = find([&](const QueueItem& it) { return it.getMediaId() == current_.getMediaId(); });
    }
    if (idx < 0) {
        log::get()->error("active item not found.");
        if (auto d = delegate_.lock())
            d->onError();
        return;
    }
    if (repeatMode_ == RepeatMode::One)
        idx = std::min<long>(idx + static_cast<long>(items_.size() / 2), static_cast<long>(items_.size()) - 1);
    beforeSize_ = static_cast<unsigned>(idx);
    afterSize_ = static_cast<unsigned>(items_.size() - static_cast<std::size_t>(idx) - 1);
    current_ = items_[static_cast<std::size_t>(idx)];
    log::get()->debug("[{}] item found. before/after:{}/{}", id_, beforeSize_, afterSize_);
    log::get()->debug("[{}] current: {}", id_, current_.getInfo());
    log::get()->debug("[{}] change state {}->{}", id_, static_cast<int>(state_), static_cast<int>(State::Loaded));
    state_ = State::Loaded;
}

}  // namespace tcng::queue
