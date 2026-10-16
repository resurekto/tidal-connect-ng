#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <vector>

#include "util/ThreadPool.hpp"

using namespace tcng::util;
using namespace std::chrono_literals;

namespace {

class BlockingTask : public tcng::util::ThreadPool::ITask {
public:
    std::promise<void> started;
    std::promise<void> release;
    std::shared_future<void> releaseFuture{release.get_future().share()};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> executed{false};

    void execute() override {
        executed = true;
        started.set_value();
        releaseFuture.wait();
    }
    void cancel() override { cancelled = true; }
};

class Task : public tcng::util::ThreadPool::ITask {
public:
    std::atomic<bool> executed{false};
    void execute() override { executed = true; }
    void cancel() override {}
};

class Collector : public tcng::util::ThreadPool::Delegate {
public:
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::uint32_t> ids;
    void onCompleted(const std::shared_ptr<tcng::util::ThreadPool::ITask>& task) override {
        std::lock_guard l(m);
        ids.push_back(task->getId());
        cv.notify_all();
    }
    bool waitCount(std::size_t n) {
        std::unique_lock l(m);
        return cv.wait_for(l, 5s, [&] { return ids.size() >= n; });
    }
};

}  // namespace

TEST_CASE("tcng::util::ThreadPool: ids start at 1, FIFO, delegate on completion") {
    tcng::util::ThreadPool pool(1);
    auto collector = std::make_shared<Collector>();
    pool.setDelegate(collector);
    pool.start();
    auto a = std::make_shared<Task>();
    auto b = std::make_shared<Task>();
    CHECK(pool.postTask(a) == 1);
    CHECK(pool.postTask(b) == 2);
    REQUIRE(collector->waitCount(2));
    CHECK(collector->ids == std::vector<std::uint32_t>{1, 2});
    CHECK(a->executed);
    pool.stop();
}

TEST_CASE("tcng::util::ThreadPool: cancelTask drops pending tasks and cancels running ones") {
    tcng::util::ThreadPool pool(1);
    auto collector = std::make_shared<Collector>();
    pool.setDelegate(collector);
    pool.start();
    auto running = std::make_shared<BlockingTask>();
    auto pending = std::make_shared<Task>();
    const auto runningId = pool.postTask(running);
    running->started.get_future().wait();
    const auto pendingId = pool.postTask(pending);
    pool.cancelTask(pendingId);
    pool.cancelTask(runningId);
    CHECK(running->cancelled);
    running->release.set_value();
    REQUIRE(collector->waitCount(1));
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(pending->executed);
    CHECK(collector->ids == std::vector<std::uint32_t>{runningId});
    pool.stop();
}

TEST_CASE("tcng::util::ThreadPool: tasks posted before start run after start") {
    tcng::util::ThreadPool pool(1);
    auto collector = std::make_shared<Collector>();
    pool.setDelegate(collector);
    auto t = std::make_shared<Task>();
    pool.postTask(t);
    pool.start();
    REQUIRE(collector->waitCount(1));
    pool.stop();
    pool.stop();  // idempotent
}
