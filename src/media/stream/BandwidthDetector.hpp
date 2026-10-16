#pragma once
// BandwidthDetector: picks the size of the next HTTP range from the measured download
// bandwidth versus the bandwidth the track needs. Loop-thread only.

#include <cstdint>
#include <deque>

namespace tcng::media {

class BandwidthDetector {
public:
    static constexpr double MIN_PREBUFFER_TIME = 30;  // seconds
    static constexpr std::size_t MAX_CHUNK_HISTORY_SIZE = 30;
    static constexpr std::uint64_t BANDWIDTH_DETECTION_RANGE_SIZE = 1024 * 1024;
    static constexpr std::uint64_t FAST_CONNECTION_MIN_RANGE_SIZE = 1024 * 1024;
    static constexpr std::uint64_t FAST_CONNECTION_MAX_RANGE_SIZE = 1024 * 1024;
    static constexpr std::uint64_t SLOW_CONNECTION_MIN_RANGE_SIZE = 1024 * 1024;
    static constexpr std::uint64_t SLOW_CONNECTION_REST_RANGE_SIZE = 1024 * 1024;
    static constexpr std::uint64_t SLOW_CONNECTION_MAX_RANGE_SIZE = 5 * 1024 * 1024;

    // 0 not initialised, 1 fast, 2 slow (first range), 3 slow (rest).
    enum class Mode : int { Uninitialized = 0, Fast = 1, SlowFirst = 2, SlowRest = 3 };

    void setDuration(double seconds) { durationMs_ = seconds * 1000.0; }
    void setPosition(double ms) { positionMs_ = ms; }
    void setFileSize(std::uint64_t size) { fileSize_ = size; }
    void reset();

    void pushNextChunk(std::uint64_t size, double timeMs);
    double bandwidth() const;               // bytes per ms, size-weighted
    std::uint64_t requiredBandwidth() const;  // fileSize / durationMs
    bool isInitialized() const { return durationMs_ > 0.0 && fileSize_ != 0; }
    std::uint64_t getRangeSize();
    Mode mode() const { return mode_; }

    std::uint64_t getSlowConnectionFirstRangeSize(double bandwidth, double positionMs) const;

private:
    struct ChunkHistoryItem {
        std::uint64_t size;
        double bandwidth;
    };

    double durationMs_ = -1.0;
    double positionMs_ = 0.0;
    std::uint64_t fileSize_ = 0;
    std::uint64_t rangeSize_ = BANDWIDTH_DETECTION_RANGE_SIZE;
    Mode mode_ = Mode::Uninitialized;
    std::deque<ChunkHistoryItem> history_;
};

}  // namespace tcng::media
