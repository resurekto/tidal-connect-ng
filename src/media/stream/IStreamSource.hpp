#pragma once
// The downloadable input of one track, as AudioWorker drives it: StreamManager (byte
// ranges of one URL) or SegmentStreamManager (segmented DASH streams from
// --python-tidal-path). makeStreamSource() picks one per StreamDescriptor.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "media/IMediaPlayer.hpp"
#include "media/audio/Interfaces.hpp"
#include "media/stream/HttpStreamFetcher.hpp"
#include "media/stream/Range.hpp"

namespace tcng::media {

class BandwidthDetector;

// StreamDescriptor: url, format name ("flac"/"aac"/"unknown"), keyId.
// TCNG: initUrl/segments/durationSeconds for segmented streams (format "flac-mp4").
struct StreamDescriptor {
    std::string url;
    std::string format;
    std::string keyId;
    std::string initUrl;
    std::vector<AudioSegment> segments;
    double durationSeconds = 0.0;
};

// One download for HttpStreamFetcher: a byte range, or the whole resource.
struct FetchRequest {
    std::string url;
    std::optional<Range> range;
};

class IStreamSource : public IInputStream {
public:
    virtual void load(const StreamDescriptor& descriptor) = 0;
    // The url for logs and MaxConnectionsStatus.
    virtual const std::string& url() const = 0;
    // The next download, or nullopt when nothing is missing at the moment.
    virtual std::optional<FetchRequest> nextFetch() = 0;
    virtual void append(const StreamChunkDescriptor& chunk) = 0;
    // Download ahead while the output buffer is full (the decoder is not waiting).
    virtual bool wantsPrefetch() const { return false; }
    // Repositions by time for sources that cannot map a time to a byte offset up front.
    // A value: the source restarted at that time (<= seconds); the decoder must be recreated
    // and skip forward. nullopt: seek by bytes through the decoder (StreamManager).
    virtual std::optional<double> restartAt(double /*seconds*/) { return std::nullopt; }
    // Track duration known up front; 0 = take it from the decoder.
    virtual double durationHint() const { return 0.0; }
};

// SegmentStreamManager for a descriptor with segments, StreamManager otherwise.
std::unique_ptr<IStreamSource> makeStreamSource(const StreamDescriptor& descriptor, BandwidthDetector& bandwidth,
                                                std::uint64_t fetchLimit);

}  // namespace tcng::media
