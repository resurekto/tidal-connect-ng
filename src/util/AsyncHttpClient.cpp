#include "util/AsyncHttpClient.hpp"

#include "util/Log.hpp"

namespace tcng::util {

HttpTask::HttpTask(HttpRequest request, bool verifyServer)
    : request_(std::move(request)), verifyServer_(verifyServer) {}

void HttpTask::execute() {
    auto client = std::make_shared<HttpClient>(true);
    client->enableServerVerification(verifyServer_);
    {
        std::lock_guard lock(mutex_);
        client_ = client;
    }
    response_ = client->request(request_);
    std::lock_guard lock(mutex_);
    client_.reset();
}

void HttpTask::cancel() {
    std::lock_guard lock(mutex_);
    if (client_)
        client_->cancel();
}

AsyncHttpClient::AsyncHttpClient(unsigned threadCount) : pool_(threadCount) {}

AsyncHttpClient::~AsyncHttpClient() {
    pool_.stop();
}

void AsyncHttpClient::start() {
    pool_.setDelegate(weak_from_this());
    pool_.start();
}

void AsyncHttpClient::stop() {
    pool_.stop();
}

std::uint32_t AsyncHttpClient::request(const HttpRequest& request, const std::shared_ptr<Delegate>& delegate) {
    if (!pool_.started()) {
        log::get()->error("ThreadPool not started.");
        return 0;
    }
    auto task = std::make_shared<HttpTask>(request, verify_);
    task->delegate_ = delegate;
    return pool_.postTask(task);
}

void AsyncHttpClient::cancel(std::uint32_t id) {
    pool_.cancelTask(id);
}

void AsyncHttpClient::onCompleted(const std::shared_ptr<ThreadPool::ITask>& task) {
    auto httpTask = std::dynamic_pointer_cast<HttpTask>(task);
    if (!httpTask)
        return;
    if (auto delegate = httpTask->delegate_.lock())
        delegate->onReceived(httpTask->getId(), httpTask->response());
}

}  // namespace tcng::util
