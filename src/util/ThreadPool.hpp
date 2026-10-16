#pragma once
// ThreadPool: a fixed set of workers drains a FIFO of tasks. postTask() hands out ids that
// start at 1 and increase per pool; after execute() the worker calls
// delegate->onCompleted(task) on the same worker thread.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tcng::util {

class ThreadPool {
public:
    // A unit of work with execute() and cancel(); its id lives with the task, under the task
    // mutex.
    class ITask {
    public:
        virtual ~ITask() = default;
        virtual void execute() = 0;
        virtual void cancel() = 0;

        std::uint32_t getId() const;
        void setId(std::uint32_t id);

    private:
        mutable std::mutex idMutex_;
        std::uint32_t id_ = 0;
    };

    class Delegate {
    public:
        virtual ~Delegate() = default;
        // Runs on the worker thread that executed the task.
        virtual void onCompleted(const std::shared_ptr<ITask>& task) = 0;
    };

    explicit ThreadPool(unsigned threadCount);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void setDelegate(const std::weak_ptr<Delegate>& delegate);

    // Spawns threadCount workers (no-op if already started).
    void start();
    // Wakes and joins all workers. Pending tasks stay queued.
    void stop();
    bool started() const;

    // Queues the task, assigns and returns its id (>= 1).
    std::uint32_t postTask(const std::shared_ptr<ITask>& task);
    // Drops a pending task, or calls cancel() on a running one.
    void cancelTask(std::uint32_t id);

private:
    void workerLoop(std::uint64_t generation);

    const unsigned threadCount_;
    std::vector<std::thread> threads_;
    std::deque<std::shared_ptr<ITask>> pending_;
    std::unordered_map<std::uint32_t, std::shared_ptr<ITask>> running_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::uint64_t generation_ = 0;  // bumped by stop(); a detached worker exits on mismatch
    std::uint32_t lastTaskId_ = 0;
    std::weak_ptr<Delegate> delegate_;
};

}  // namespace tcng::util
