#pragma once
// AudioPipeline: one track = HttpStreamFetcher + AudioDecoderFactory + BandwidthDetector +
// AudioOutputBuffer + AudioWorker. Built by MediaPlayer::prepare/prepareNext; work()/seek() run on the loop
// thread, readInterleavedPcmData()/getFillGrade() on the audio callback thread.

#include <cstddef>
#include <cstdint>

#include "media/IMediaPlayer.hpp"
#include "media/audio/AudioDecoderFactory.hpp"
#include "media/audio/AudioOutputBuffer.hpp"
#include "media/audio/AudioWorker.hpp"
#include "media/stream/BandwidthDetector.hpp"
#include "media/stream/HttpStreamFetcher.hpp"

namespace tcng::media {

class AudioPipeline : public PipelineDecoderClient {
public:
    AudioPipeline(const AudioItem& item, std::uint64_t streamFetchLimit, std::size_t audioOutputBufferSize);
    ~AudioPipeline() override;
    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    // StreamDescriptor format for an AudioItem: "flac", "aac" or "unknown" (probe).
    static StreamDescriptor descriptorFor(const AudioItem& item);

    // Loop thread
    void work() { worker_.work(); }
    void seek(double seconds);
    bool complete() const { return worker_.complete(); }
    bool drained() const { return output_.framesAvailableForRead() == 0; }
    double getDuration() const { return duration_; }  // seconds, 0 until known
    const AudioItem& getAudioItem() const { return item_; }
    AudioMetadata getAudioMetadata() const { return output_.getAudioMetadata(); }
    void setPlaybackMetadataCallback(AudioOutputBuffer::MetadataCallback cb) {
        output_.setPlaybackMetadataCallback(std::move(cb));
    }
    DecoderStatus decoderStatus() const { return decoderStatus_; }
    bool maxConnectionsReached() const { return maxConnectionsReached_; }
    const MaxConnectionsStatus& maxConnectionsStatus() const { return maxConnectionsStatus_; }

    // Audio thread
    std::size_t readInterleavedPcmData(void* out, std::size_t frames) {
        return output_.readInterleavedPcmData(out, frames);
    }
    double getFillGrade() const { return output_.getFillGrade(); }

    // PipelineDecoderClient
    void onMaxConnectionsReached(const MaxConnectionsStatus& status) override;
    void onDecoderError(DecoderStatus status) override { decoderStatus_ = status; }
    void onDuration(double seconds) override { duration_ = seconds; }
    void onStreamFormat(StreamFormat format) override { streamFormat_ = format; }

private:
    double duration_ = 0.0;
    DecoderStatus decoderStatus_ = DecoderStatus::Success;
    StreamFormat streamFormat_ = StreamFormat::None;
    MaxConnectionsStatus maxConnectionsStatus_;
    bool maxConnectionsReached_ = false;
    HttpStreamFetcher fetcher_;
    AudioDecoderFactory factory_;
    BandwidthDetector bandwidth_;
    AudioOutputBuffer output_;
    AudioWorker worker_;
    AudioItem item_;
};

}  // namespace tcng::media
