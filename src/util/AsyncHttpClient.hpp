#pragma once
// AsyncHttpClient owns a ThreadPool (all users pass 1 thread); each request() posts an
// HttpTask whose result is delivered to the request's Delegate on the pool thread.

#include <cstdint>
#include <memory>
#include <mutex>

#include "util/ThreadPool.hpp"
#include "util/HttpClient.hpp"

namespace tcng::util {

class AsyncHttpClientDelegate {
public:
    virtual ~AsyncHttpClientDelegate() = default;
    // Runs on the AsyncHttpClient pool thread. id = value returned by request().
    virtual void onReceived(std::uint32_t id, const HttpResponse& response) = 0;
};

class HttpTask : public ThreadPool::ITask {
public:
    HttpTask(HttpRequest request, bool verifyServer);

    void execute() override;  // new HttpClient(true), request(), keep the response
    void cancel() override;   // aborts the in-flight transfer

    const HttpResponse& response() const { return response_; }

private:
    HttpRequest request_;
    bool verifyServer_;
    std::mutex mutex_;
    std::shared_ptr<HttpClient> client_;
    HttpResponse response_;

    friend class AsyncHttpClient;
    std::weak_ptr<AsyncHttpClientDelegate> delegate_;
};

class AsyncHttpClient : public ThreadPool::Delegate, public std::enable_shared_from_this<AsyncHttpClient> {
public:
    using Delegate = AsyncHttpClientDelegate;

    explicit AsyncHttpClient(unsigned threadCount);
    ~AsyncHttpClient() override;

    void start();
    void stop();
    void setTimeout(unsigned seconds) { timeout_ = seconds; }  // stored, unused
    void enableServerVerification(bool enable) { verify_ = enable; }

    // Returns the task id, or 0 if the pool is not started ("ThreadPool not started.").
    std::uint32_t request(const HttpRequest& request, const std::shared_ptr<Delegate>& delegate);
    void cancel(std::uint32_t id);

    void onCompleted(const std::shared_ptr<ThreadPool::ITask>& task) override;

private:
    ThreadPool pool_;
    unsigned timeout_ = 0;
    bool verify_ = true;
};

}  // namespace tcng::util
