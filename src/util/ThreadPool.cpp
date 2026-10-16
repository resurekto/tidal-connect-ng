#include "util/ThreadPool.hpp"

#include <algorithm>

#include "util/Log.hpp"

namespace tcng::util {

std::uint32_t ThreadPool::ITask::getId() const {
    std::lock_guard lock(idMutex_);
    return id_;
}

void ThreadPool::ITask::setId(std::uint32_t id) {
    std::lock_guard lock(idMutex_);
    id_ = id;
}

ThreadPool::ThreadPool(unsigned threadCount) : threadCount_(threadCount) {}

ThreadPool::~ThreadPool() {
    stop();
}

void ThreadPool::setDelegate(const std::weak_ptr<Delegate>& delegate) {
    std::lock_guard lock(mutex_);
    delegate_ = delegate;
}

void ThreadPool::start() {
    std::lock_guard lock(mutex_);
    if (!threads_.empty())
        return;
    stop_ = false;
    for (unsigned i = 0; i < threadCount_; ++i)
        threads_.emplace_back([this, gen = generation_] { workerLoop(gen); });
}

void ThreadPool::stop() {
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        ++generation_;
        threads.swap(threads_);
        cv_.notify_all();
    }
    for (auto& t : threads) {
        if (t.get_id() == std::this_thread::get_id())
            t.detach();  // stopped from a delegate callback on this very worker
        else if (t.joinable())
            t.join();
    }
    std::lock_guard lock(mutex_);
    stop_ = false;
}

bool ThreadPool::started() const {
    std::lock_guard lock(mutex_);
    return !threads_.empty();
}

std::uint32_t ThreadPool::postTask(const std::shared_ptr<ITask>& task) {
    std::lock_guard lock(mutex_);
    if (++lastTaskId_ == 0)  // wrapped: 0 means "no task" to the engines
        lastTaskId_ = 1;
    task->setId(lastTaskId_);
    pending_.push_back(task);
    cv_.notify_one();
    return lastTaskId_;
}

void ThreadPool::cancelTask(std::uint32_t id) {
    std::shared_ptr<ITask> runningTask;
    {
        std::lock_guard lock(mutex_);
        auto it = std::find_if(pending_.begin(), pending_.end(),
                               [id](const auto& t) { return t->getId() == id; });
        if (it != pending_.end()) {
            pending_.erase(it);
            return;
        }
        if (auto r = running_.find(id); r != running_.end())
            runningTask = r->second;
    }
    if (runningTask)
        runningTask->cancel();
}

void ThreadPool::workerLoop(std::uint64_t generation) {
    for (;;) {
        std::shared_ptr<ITask> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || generation != generation_ || !pending_.empty(); });
            if (stop_ || generation != generation_)
                return;
            task = pending_.front();
            pending_.pop_front();
            running_[task->getId()] = task;
        }
        task->execute();
        std::shared_ptr<Delegate> delegate;
        {
            std::lock_guard lock(mutex_);
            delegate = delegate_.lock();
        }
        if (delegate)
            delegate->onCompleted(task);
        std::lock_guard lock(mutex_);
        running_.erase(task->getId());
    }
}

}  // namespace tcng::util
