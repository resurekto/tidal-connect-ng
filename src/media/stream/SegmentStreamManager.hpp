#pragma once
// TCNG: IStreamSource for segmented (DASH) FLAC streams from --python-tidal-path. Downloads the init
// segment and then the media segments in order, unwraps FLAC from the MP4 (Mp4Flac.hpp) as
// the bytes arrive and appends it to a BufferStream, so the decoder sees one native FLAC
// stream. The size becomes known when the last segment is in.
//
// Latency: right after load/restartAt the first segment comes as a small byte range
// (kFirstRangeBytes) and then the rest of it in one range, so decoding starts after a few
// KB; later segments are fetched whole (one request each). The FLAC header is kept, so a
// restart does not download the init segment again.
//
// restartAt(t): restarts the stream as header + the segment containing t (from the segment
// durations), so a seek never waits for the segments before it.

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "media/stream/BufferStream.hpp"
#include "media/stream/IStreamSource.hpp"
#include "media/stream/Mp4Flac.hpp"

namespace tcng::media {

class SegmentStreamManager : public IStreamSource {
public:
    // Segments downloaded ahead of the one being decoded while the output buffer is full.
    static constexpr std::size_t kSegmentsAhead = 2;
    // First range of the first segment after load/restartAt.
    static constexpr std::uint64_t kFirstRangeBytes = 64 * 1024;
    // Range size while a segment's total size is unknown (Content-Range without it).
    static constexpr std::uint64_t kUnknownSizeRangeBytes = 8 * 1024 * 1024;

    // IStreamSource
    void load(const StreamDescriptor& descriptor) override;
    const std::string& url() const override { return currentUrl_; }
    std::optional<FetchRequest> nextFetch() override;
    void append(const StreamChunkDescriptor& chunk) override;
    bool wantsPrefetch() const override;
    std::optional<double> restartAt(double seconds) override;
    double durationHint() const override { return descriptor_.durationSeconds; }

    // IInputStream
    void setDecodedPosition(std::uint64_t position) override;
    void setStreamFormat(StreamFormat format) override { formatOverride_ = format; }
    StreamFormat format() const override;
    bool endOfStream() const override { return stream_.endOfStream(); }
    bool complete() const override { return stream_.complete(); }
    std::uint64_t availableBytes() const override { return stream_.availableBytes(); }
    std::uint64_t position() const override { return stream_.position(); }
    std::uint64_t size() const override { return stream_.size(); }
    std::size_t read(char* dst, std::size_t n) override { return stream_.read(dst, n); }
    std::int64_t seek(std::uint64_t position) override { return stream_.seek(position); }

    // For tests / logs.
    std::size_t nextSegment() const { return nextSegment_; }
    const BufferStream& bufferStream() const { return stream_; }

private:
    void restart(std::size_t segment);
    void appendBytes(std::vector<char> bytes);
    void appendSegmentBytes(const char* data, std::size_t size);
    void finishSegment();

    StreamDescriptor descriptor_;
    BufferStream stream_;
    StreamFormat formatOverride_ = StreamFormat::None;
    std::vector<char> header_;      // "fLaC" + metadata, once the init segment is in
    bool headerPending_ = true;     // the init segment is the next download
    std::size_t nextSegment_ = 0;   // next media segment to download (or the one in progress)
    std::uint64_t writeOffset_ = 0;
    std::deque<std::uint64_t> segmentEnds_;  // end offsets (exclusive) of the appended segments
    std::string currentUrl_;
    // The segment in progress.
    MdatStream mdat_;
    std::uint64_t segmentReceived_ = 0;  // raw bytes of it received
    std::uint64_t segmentSize_ = 0;      // its size, from Content-Range; 0 = unknown
    bool firstRangePending_ = true;      // ask for kFirstRangeBytes first
};

}  // namespace tcng::media
