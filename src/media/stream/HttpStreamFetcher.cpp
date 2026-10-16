#include "media/stream/HttpStreamFetcher.hpp"

#include <chrono>

#include "util/Log.hpp"

namespace tcng::media {

HttpStreamFetcher::~HttpStreamFetcher() {
    abort();
    if (thread_.joinable())
        thread_.join();
}

void HttpStreamFetcher::fetch(const std::string& url, const std::optional<Range>& range) {
    std::thread previous;
    std::shared_ptr<Operation> op;
    {
        std::lock_guard lock(mutex_);
        if (state_ == State::Fetching && url == url_ && range == range_)
            return;
        if (op_)
            op_->abort();
        if (url != url_) {
            url_ = url;
            attempts_ = 0;
        }
        range_ = range;
        state_ = State::Fetching;
        previous.swap(thread_);
        op = std::make_shared<Operation>();
        op_ = op;
    }
    if (previous.joinable())
        previous.join();  // outside the mutex: run() takes it to publish its result
    std::lock_guard lock(mutex_);
    thread_ = std::thread(&HttpStreamFetcher::run, this, url, range, op);
}

void HttpStreamFetcher::abort() {
    std::lock_guard lock(mutex_);
    if (op_)
        op_->abort();
    state_ = State::Aborted;
}

HttpStreamFetcher::State HttpStreamFetcher::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

StreamChunkDescriptor HttpStreamFetcher::chunkDescriptor() const {
    std::lock_guard lock(mutex_);
    return chunk_;
}

void HttpStreamFetcher::run(std::string url, std::optional<Range> range, std::shared_ptr<Operation> op) {
    const auto start = std::chrono::system_clock::now();
    if (range)
        op->send(url, {{"Range", buildRangeValue(*range)}}, static_cast<std::size_t>(range->size()));
    else
        op->send(url, {}, 0);
    const auto elapsed = std::chrono::system_clock::now() - start;

    std::lock_guard lock(mutex_);
    if (op->status() == Operation::Status::Aborted || state_ == State::Aborted || op != op_)
        return;
    chunk_ = StreamChunkDescriptor{};
    chunk_.range = range.value_or(Range{});
    chunk_.url = url;
    chunk_.downloadMs = std::chrono::duration<double, std::milli>(elapsed).count();
    chunk_.body = op->body();
    chunk_.httpStatus = op->httpStatus();
    chunk_.statusMessage = op->statusMessage();
    const auto& headers = op->responseHeaders();
    if (auto it = headers.find("content-range"); it != headers.end())
        chunk_.totalLength = getContentRangeLength(it->second).value_or(0);
    else if (range)
        log::get()->warn("Content-Range is missing from response!");
    else
        chunk_.totalLength = chunk_.body ? chunk_.body->size() : 0;

    const bool httpOk = chunk_.httpStatus >= 200 && chunk_.httpStatus < 300;
    if (httpOk && op->status() == Operation::Status::Ok) {
        state_ = State::Done;
        attempts_ = 0;
        return;
    }
    state_ = op->status() == Operation::Status::Timeout ? State::Timeout : State::Error;
    ++attempts_;
    log::get()->warn("stream fetch failed: http={} {} (attempt {}/{})", chunk_.httpStatus, chunk_.statusMessage,
                   attempts_, maxConnectionAttempts_);
    if (attempts_ >= maxConnectionAttempts_)
        state_ = State::MaxAttemptsReached;
}

}  // namespace tcng::media
