#include "media/stream/BandwidthDetector.hpp"

#include <algorithm>

#include "util/Log.hpp"

namespace tcng::media {

void BandwidthDetector::reset() {
    durationMs_ = -1.0;
    positionMs_ = 0.0;
    fileSize_ = 0;
    rangeSize_ = BANDWIDTH_DETECTION_RANGE_SIZE;
    mode_ = Mode::Uninitialized;
    history_.clear();
}

void BandwidthDetector::pushNextChunk(std::uint64_t size, double timeMs) {
    const double bw = timeMs > 1.0 ? static_cast<double>(size) / timeMs : static_cast<double>(size);
    history_.push_back({size, bw});
    while (history_.size() > MAX_CHUNK_HISTORY_SIZE)
        history_.pop_front();
}

double BandwidthDetector::bandwidth() const {
    double weighted = 0.0;
    double total = 0.0;
    for (const auto& item : history_) {
        weighted += static_cast<double>(item.size) * item.bandwidth;
        total += static_cast<double>(item.size);
    }
    return total > 0.0 ? weighted / total : 0.0;
}

std::uint64_t BandwidthDetector::requiredBandwidth() const {
    const auto duration = static_cast<std::uint64_t>(std::max(durationMs_, 0.0));
    return duration ? fileSize_ / duration : 0;
}

// MIN_PREBUFFER_TIME is in seconds; the duration is in milliseconds.
std::uint64_t BandwidthDetector::getSlowConnectionFirstRangeSize(double bw, double positionMs) const {
    double t = durationMs_ - MIN_PREBUFFER_TIME * 1000.0;
    if (positionMs < durationMs_)
        t -= positionMs;
    t = std::max(t, 0.0);
    const double downloadable = t * bw;
    const double size = static_cast<double>(fileSize_) - downloadable;
    if (size <= static_cast<double>(SLOW_CONNECTION_MIN_RANGE_SIZE))
        return SLOW_CONNECTION_MIN_RANGE_SIZE;
    if (size >= static_cast<double>(SLOW_CONNECTION_MAX_RANGE_SIZE))
        return SLOW_CONNECTION_MAX_RANGE_SIZE;
    return static_cast<std::uint64_t>(size);
}

std::uint64_t BandwidthDetector::getRangeSize() {
    const double bw = bandwidth();
    if (bw >= 0.0 && isInitialized()) {
        if (static_cast<double>(requiredBandwidth()) <= bw) {
            mode_ = Mode::Fast;
            rangeSize_ = std::min(rangeSize_ * 2, FAST_CONNECTION_MAX_RANGE_SIZE);
            rangeSize_ = std::max(rangeSize_, FAST_CONNECTION_MIN_RANGE_SIZE);
        } else if (mode_ == Mode::SlowFirst || mode_ == Mode::SlowRest) {
            mode_ = Mode::SlowRest;
            rangeSize_ = SLOW_CONNECTION_REST_RANGE_SIZE;
        } else {
            mode_ = Mode::SlowFirst;
            rangeSize_ = getSlowConnectionFirstRangeSize(bw, positionMs_);
        }
    } else {
        mode_ = Mode::Uninitialized;
        rangeSize_ = BANDWIDTH_DETECTION_RANGE_SIZE;
    }
    log::get()->trace("BandwidthDetector: range={} bandwidth={} required={} size={} duration={}", rangeSize_, bw,
                    requiredBandwidth(), fileSize_, durationMs_);
    return rangeSize_;
}

}  // namespace tcng::media
