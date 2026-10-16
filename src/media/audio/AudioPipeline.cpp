#include "media/audio/AudioPipeline.hpp"

#include "util/Log.hpp"

namespace tcng::media {

StreamDescriptor AudioPipeline::descriptorFor(const AudioItem& item) {
    StreamDescriptor d;
    d.url = item.url;
    d.keyId = item.keyId;
    d.initUrl = item.initUrl;
    d.segments = item.segments;
    d.durationSeconds = item.durationSeconds;
    switch (item.audioCodec) {
    case audio_codec::kFlac:
        d.format = "flac";
        break;
    case audio_codec::kAacLc:
    case audio_codec::kHeAac:
        d.format = "aac";
        break;
    default:
        d.format = "unknown";
        break;
    }
    return d;
}

AudioPipeline::AudioPipeline(const AudioItem& item, std::uint64_t streamFetchLimit, std::size_t audioOutputBufferSize)
    : output_(audioOutputBufferSize),
      worker_(*this, output_, factory_, bandwidth_, fetcher_, streamFetchLimit),
      item_(item) {
    worker_.load(descriptorFor(item));
}

AudioPipeline::~AudioPipeline() {
    worker_.stop();
}

void AudioPipeline::seek(double seconds) {
    worker_.seek(seconds);
    output_.flush();
}

void AudioPipeline::onMaxConnectionsReached(const MaxConnectionsStatus& status) {
    maxConnectionsStatus_ = status;
    maxConnectionsReached_ = true;
}

}  // namespace tcng::media
