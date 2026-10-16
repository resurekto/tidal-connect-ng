#pragma once
// HttpStreamFetcher: downloads one byte range at a time on its own thread. fetch() starts a
// new std::thread per range; the AudioWorker polls state() from the loop thread and
// re-issues fetch() to retry (no retry loop or backoff here). After maxConnectionAttempts
// (3) consecutive failures the state becomes MaxAttemptsReached.
//
// TCNG: a fetch without a range downloads the whole resource (a DASH media segment).

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "media/stream/Operation.hpp"
#include "media/stream/Range.hpp"

namespace tcng::media {

struct StreamChunkDescriptor {
    Range range;                    // as requested; {0, 0} for a whole-resource fetch
    std::string url;
    double downloadMs = 0.0;
    std::shared_ptr<const std::vector<char>> body;
    std::uint64_t totalLength = 0;  // from Content-Range; 0 = unknown
    long httpStatus = 0;
    std::string statusMessage;
};

class HttpStreamFetcher {
public:
    // 0 idle, 1 fetching, 2 done-OK, 3 aborted, 4 timeout, 5 error, 6 max attempts reached.
    enum class State : int { Idle = 0, Fetching = 1, Done = 2, Aborted = 3, Timeout = 4, Error = 5, MaxAttemptsReached = 6 };

    HttpStreamFetcher() = default;
    ~HttpStreamFetcher();
    HttpStreamFetcher(const HttpStreamFetcher&) = delete;
    HttpStreamFetcher& operator=(const HttpStreamFetcher&) = delete;

    void fetch(const std::string& url, const std::optional<Range>& range);
    void abort();
    State state() const;
    StreamChunkDescriptor chunkDescriptor() const;
    int maxConnectionAttempts() const { return maxConnectionAttempts_; }

private:
    void run(std::string url, std::optional<Range> range, std::shared_ptr<Operation> op);

    mutable std::mutex mutex_;
    State state_ = State::Idle;
    std::string url_;
    std::optional<Range> range_;
    int attempts_ = 0;
    const int maxConnectionAttempts_ = 3;
    std::shared_ptr<Operation> op_;
    std::thread thread_;
    StreamChunkDescriptor chunk_;
};

}  // namespace tcng::media
