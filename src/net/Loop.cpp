#include "net/Loop.hpp"

#include <libwebsockets.h>

#include <future>
#include <stdexcept>

#include "util/Log.hpp"

namespace tcng::net {

struct Loop::Timer {
    lws_sorted_usec_list_t sul{};  // must stay first: lws hands us back this pointer
    Loop* loop = nullptr;
    TimerId id = 0;
    std::function<void()> fn;
};

namespace {

void lwsLogEmit(int level, const char* line) {
    std::string_view text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.remove_suffix(1);
    auto logger = log::get();
    if (level & LLL_ERR)
        logger->error("lws: {}", std::string(text));
    else if (level & LLL_WARN)
        logger->warn("lws: {}", std::string(text));
    else if (level & (LLL_NOTICE | LLL_USER))
        logger->info("lws: {}", std::string(text));
    else
        logger->debug("lws: {}", std::string(text));
}

int loopCallback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len);

const lws_protocols kLoopProtocols[] = {
    {"tcng-loop", loopCallback, 0, 0, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM,
};

int loopCallback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len) {
    return Loop::dispatch(wsi, static_cast<int>(reason), user, in, len);
}

}  // namespace

Loop::Loop() {
    lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_USER, lwsLogEmit);

    lws_context_creation_info info{};
    info.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS | LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.user = this;
    context_ = lws_create_context(&info);
    if (!context_)
        throw std::runtime_error("lws_create_context failed");

    // A listen-less vhost that carries our protocol, so the loop receives
    // LWS_CALLBACK_EVENT_WAIT_CANCELLED (the wakeup for post()) even when no
    // server vhost exists.
    lws_context_creation_info vinfo{};
    vinfo.port = CONTEXT_PORT_NO_LISTEN;
    vinfo.protocols = kLoopProtocols;
    vinfo.vhost_name = "tcng-loop";
    vinfo.user = this;
    if (!lws_create_vhost(context_, &vinfo))
        throw std::runtime_error("lws_create_vhost (loop) failed");
}

Loop::~Loop() {
    stop();
    if (context_)
        lws_context_destroy(context_);
}

void Loop::start() {
    if (running_.exchange(true))
        return;
    thread_ = std::thread([this] { run(); });
}

void Loop::stop() {
    if (!running_.exchange(false))
        return;
    stopping_ = true;
    lws_cancel_service(context_);
    if (thread_.joinable())
        thread_.join();
}

void Loop::run() {
    threadId_ = std::this_thread::get_id();
    while (!stopping_) {
        if (lws_service(context_, 0) < 0)
            break;
    }
    // Leave pending work and timers behind: their owners are being torn down too.
    for (auto& [id, timer] : timers_)
        lws_sul_cancel(&timer->sul);
    timers_.clear();
}

bool Loop::inLoopThread() const {
    return std::this_thread::get_id() == threadId_;
}

void Loop::post(std::function<void()> fn) {
    {
        std::lock_guard lock(mutex_);
        posted_.push_back(std::move(fn));
    }
    lws_cancel_service(context_);
}

void Loop::call(const std::function<void()>& fn) {
    if (!running_ || inLoopThread()) {
        fn();
        return;
    }
    std::promise<void> done;
    auto future = done.get_future();
    post([&] {
        try {
            fn();
            done.set_value();
        } catch (...) {
            done.set_exception(std::current_exception());
        }
    });
    future.get();
}

void Loop::drain() {
    std::vector<std::function<void()>> work;
    {
        std::lock_guard lock(mutex_);
        work.swap(posted_);
    }
    for (auto& fn : work)
        fn();
}

TimerId Loop::schedule(std::chrono::milliseconds delay, std::function<void()> fn) {
    const TimerId id = nextTimerId_++;
    auto arm = [this, id, delay, fn = std::move(fn)]() mutable {
        auto timer = std::make_unique<Timer>();
        timer->loop = this;
        timer->id = id;
        timer->fn = std::move(fn);
        lws_sul_schedule(context_, 0, &timer->sul, &Loop::onTimer,
                         static_cast<lws_usec_t>(delay.count()) * LWS_US_PER_MS);
        timers_[id] = std::move(timer);
    };
    if (running_ && inLoopThread())
        arm();
    else
        post(std::move(arm));
    return id;
}

void Loop::cancel(TimerId id) {
    auto disarm = [this, id] {
        auto it = timers_.find(id);
        if (it == timers_.end())
            return;
        lws_sul_cancel(&it->second->sul);
        timers_.erase(it);
    };
    if (running_ && inLoopThread())
        disarm();
    else
        post(disarm);
}

void Loop::onTimer(lws_sorted_usec_list* sul) {
    auto* timer = reinterpret_cast<Timer*>(sul);
    Loop* loop = timer->loop;
    auto node = loop->timers_.extract(timer->id);
    if (node.empty())
        return;
    node.mapped()->fn();
}

lws_vhost* Loop::createVhost(lws_context_creation_info& info) {
    lws_vhost* vhost = nullptr;
    call([&] { vhost = lws_create_vhost(context_, &info); });
    return vhost;
}

int Loop::dispatch(lws* wsi, int reason, void*, void*, std::size_t) {
    if (reason == LWS_CALLBACK_EVENT_WAIT_CANCELLED) {
        auto* loop = static_cast<Loop*>(lws_context_user(lws_get_context(wsi)));
        if (loop)
            loop->drain();
    }
    return 0;
}

}  // namespace tcng::net
