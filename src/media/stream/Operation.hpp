#pragma once
// Operation, libcurl backend: one blocking GET of a byte range from the CDN,
// on a fresh curl handle each time.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tcng::media {

class Operation {
public:
    // Op status: 0 idle, 1 running, 2 ok, 3 timeout, 4 error, 5 aborted.
    enum class Status : int { Idle = 0, Running = 1, Ok = 2, Timeout = 3, Error = 4, Aborted = 5 };

    static constexpr const char* USER_AGENT = "TidalConnect/1.1.3";

    // Blocking. `headers` are sent as "Name: value"; reserve = expected body size.
    void send(const std::string& url, const std::map<std::string, std::string>& headers, std::size_t reserve);
    // Thread-safe: makes the transfer stop at the next progress callback.
    void abort();

    Status status() const { return status_.load(); }
    long httpStatus() const { return httpStatus_; }
    const std::string& statusMessage() const { return statusMessage_; }
    // Response headers with lower-cased names.
    const std::map<std::string, std::string>& responseHeaders() const { return responseHeaders_; }
    std::shared_ptr<std::vector<char>> body() const { return body_; }

private:
    std::atomic<Status> status_{Status::Idle};
    long httpStatus_ = 0;
    std::string statusMessage_;
    std::map<std::string, std::string> responseHeaders_;
    std::shared_ptr<std::vector<char>> body_ = std::make_shared<std::vector<char>>();
};

}  // namespace tcng::media
