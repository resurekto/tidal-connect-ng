#include "media/stream/SegmentStreamManager.hpp"

#include <memory>

#include "util/Log.hpp"
#include "media/stream/StreamManager.hpp"

namespace tcng::media {

std::unique_ptr<IStreamSource> makeStreamSource(const StreamDescriptor& descriptor, BandwidthDetector& bandwidth,
                                                std::uint64_t fetchLimit) {
    if (!descriptor.segments.empty())
        return std::make_unique<SegmentStreamManager>();
    return std::make_unique<StreamManager>(bandwidth, fetchLimit);
}

void SegmentStreamManager::load(const StreamDescriptor& descriptor) {
    descriptor_ = descriptor;
    header_.clear();
    restart(0);
}

void SegmentStreamManager::restart(std::size_t segment) {
    stream_.clear();
    writeOffset_ = 0;
    segmentEnds_.clear();
    nextSegment_ = segment;
    mdat_.reset();
    segmentReceived_ = 0;
    segmentSize_ = 0;
    firstRangePending_ = true;
    headerPending_ = header_.empty();
    if (!headerPending_)
        appendBytes(header_);  // the same for the whole track: no download
    currentUrl_ = descriptor_.initUrl;
}

StreamFormat SegmentStreamManager::format() const {
    return formatOverride_ != StreamFormat::None ? formatOverride_ : StreamFormat::Flac;
}

std::optional<FetchRequest> SegmentStreamManager::nextFetch() {
    if (headerPending_) {
        currentUrl_ = descriptor_.initUrl;
        return FetchRequest{currentUrl_, std::nullopt};
    }
    if (nextSegment_ >= descriptor_.segments.size())
        return std::nullopt;
    currentUrl_ = descriptor_.segments[nextSegment_].url;
    if (segmentReceived_ != 0) {  // the rest of a segment started with a small range
        const std::uint64_t end = segmentSize_ ? segmentSize_ - 1 : segmentReceived_ + kUnknownSizeRangeBytes - 1;
        return FetchRequest{currentUrl_, Range{segmentReceived_, end}};
    }
    if (firstRangePending_)
        return FetchRequest{currentUrl_, Range{0, kFirstRangeBytes - 1}};
    return FetchRequest{currentUrl_, std::nullopt};
}

void SegmentStreamManager::appendBytes(std::vector<char> bytes) {
    if (bytes.empty())
        return;
    const std::uint64_t n = bytes.size();
    stream_.append(Buffer::make(writeOffset_, std::make_shared<const std::vector<char>>(std::move(bytes))));
    writeOffset_ += n;
}

void SegmentStreamManager::appendSegmentBytes(const char* data, std::size_t size) {
    std::vector<char> frames;
    mdat_.feed(data, size, frames);
    appendBytes(std::move(frames));
    segmentReceived_ += size;
}

void SegmentStreamManager::finishSegment() {
    if (mdat_.failed())
        log::get()->warn("SegmentStreamManager: malformed box in segment {}", nextSegment_);
    segmentEnds_.push_back(writeOffset_);
    ++nextSegment_;
    mdat_.reset();
    segmentReceived_ = 0;
    segmentSize_ = 0;
}

void SegmentStreamManager::append(const StreamChunkDescriptor& chunk) {
    // A chunk for anything but the outstanding download is stale (e.g. from before a restart).
    if (chunk.url != currentUrl_ || !chunk.body || chunk.body->empty()) {
        log::get()->debug("SegmentStreamManager: dropping chunk for {}", chunk.url);
        return;
    }
    const auto& body = *chunk.body;
    if (headerPending_) {
        auto header = headerFromInit(body.data(), body.size());
        if (header) {
            header_ = *header;
        } else {
            // The raw bytes make FLACDecoder report corrupt metadata, failing the track.
            log::get()->error("SegmentStreamManager: no FLAC (dfLa) in the init segment");
            header = body;
        }
        appendBytes(std::move(*header));
        headerPending_ = false;
    } else {
        // 206: bytes [range.begin, ...] of totalLength. 200 (whole resource, also when a range
        // was asked for but ignored): bytes [0, size).
        const bool partial = chunk.httpStatus == 206;
        const std::uint64_t begin = partial ? chunk.range.begin : 0;
        if (begin > segmentReceived_) {
            log::get()->debug("SegmentStreamManager: dropping out-of-order range at {}", begin);
            return;
        }
        const std::uint64_t skip = segmentReceived_ - begin;  // already have these
        if (skip < body.size())
            appendSegmentBytes(body.data() + skip, body.size() - static_cast<std::size_t>(skip));
        firstRangePending_ = false;
        bool done = true;
        if (partial && chunk.totalLength != 0) {
            segmentSize_ = chunk.totalLength;
            done = segmentReceived_ >= segmentSize_;
        } else if (partial) {
            // No total in Content-Range: the segment ends with the first short range.
            done = body.size() < chunk.range.size();
        }
        if (done)
            finishSegment();
    }
    if (nextSegment_ >= descriptor_.segments.size() && !headerPending_)
        stream_.setSize(writeOffset_);  // all in: the decoder can see the end
}

bool SegmentStreamManager::wantsPrefetch() const {
    if (headerPending_ || nextSegment_ >= descriptor_.segments.size())
        return false;
    std::size_t unread = 0;
    for (const auto end : segmentEnds_)
        if (end > stream_.position())
            ++unread;
    return unread <= kSegmentsAhead;
}

std::optional<double> SegmentStreamManager::restartAt(double seconds) {
    const auto& segs = descriptor_.segments;
    std::size_t k = 0;
    double start = 0.0;
    for (; k + 1 < segs.size(); ++k) {
        if (start + segs[k].durationSeconds > seconds)
            break;
        start += segs[k].durationSeconds;
    }
    log::get()->debug("SegmentStreamManager: restart at segment {} ({}s) for {}s", k, start, seconds);
    restart(k);
    return start;
}

void SegmentStreamManager::setDecodedPosition(std::uint64_t position) {
    stream_.shrink(position);
    while (!segmentEnds_.empty() && segmentEnds_.front() <= position && segmentEnds_.size() > 1)
        segmentEnds_.pop_front();
}

}  // namespace tcng::media
