#pragma once
// The single libwebsockets service loop. It owns the lws_context and the thread that
// services it. Every socket (phone WSS, spkcon Unix socket) and every timer lives on
// this thread; other threads hand work over with post().

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct lws_context;
struct lws_vhost;
struct lws;
struct lws_sorted_usec_list;
struct lws_context_creation_info;

namespace tcng::net {

using TimerId = std::uint64_t;

class Loop {
public:
    Loop();
    ~Loop();
    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;

    // Starts the service thread. Vhosts may be created before or after.
    void start();
    // Stops servicing and joins the thread. Safe to call more than once.
    void stop();

    // Runs fn on the loop thread. Thread-safe.
    void post(std::function<void()> fn);
    // Runs fn on the loop thread and waits for it (runs inline if already there).
    void call(const std::function<void()>& fn);

    // One-shot timer on the loop thread. Thread-safe.
    TimerId schedule(std::chrono::milliseconds delay, std::function<void()> fn);
    void cancel(TimerId id);

    bool inLoopThread() const;
    lws_context* context() const { return context_; }

    // Creates a vhost on the loop thread (lws vhost creation is not thread-safe).
    lws_vhost* createVhost(lws_context_creation_info& info);

    // lws protocol callback for the loop's own vhost (internal).
    static int dispatch(struct lws* wsi, int reason, void* user, void* in, std::size_t len);

private:
    struct Timer;
    static void onTimer(struct lws_sorted_usec_list* sul);
    void drain();
    void run();

    lws_context* context_ = nullptr;
    std::thread thread_;
    std::thread::id threadId_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    std::mutex mutex_;
    std::vector<std::function<void()>> posted_;

    // Loop-thread only.
    std::map<TimerId, std::unique_ptr<Timer>> timers_;
    std::atomic<TimerId> nextTimerId_{1};
};

}  // namespace tcng::net
