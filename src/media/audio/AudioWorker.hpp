#pragma once
// AudioWorker: the per-track state machine, ticked from the MediaPlayer loop thread. It
// interleaves range fetches (at most one in flight) and decoding (200 ms per tick, only
// while the output buffer has room for 400 ms).
//
// TCNG: the input is an IStreamSource chosen per track (StreamManager, or SegmentStreamManager
// for segmented streams); sources may prefetch while the output is full and seek by
// restarting at a time (restartAt) instead of by bytes.

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "media/audio/AudioDecoderFactory.hpp"
#include "media/audio/AudioOutputBuffer.hpp"
#include "media/audio/Interfaces.hpp"
#include "media/stream/BandwidthDetector.hpp"
#include "media/stream/HttpStreamFetcher.hpp"
#include "media/stream/IStreamSource.hpp"

namespace tcng::media {

struct MaxConnectionsStatus {
    std::string url;
    long status = 0;
    std::string message;
};

// PipelineDecoderClient (implemented by AudioPipeline).
class PipelineDecoderClient {
public:
    virtual ~PipelineDecoderClient() = default;
    virtual void onMaxConnectionsReached(const MaxConnectionsStatus& status) = 0;
    virtual void onDecoderError(DecoderStatus status) = 0;
    virtual void onDuration(double seconds) = 0;
    virtual void onStreamFormat(StreamFormat format) = 0;
};

class AudioWorker {
public:
    enum class State : int { Uninitialized = 0, Idle = 1, Fetch = 2, Underrun = 3, Decode = 4, Stopped = 5, Complete = 6 };

    AudioWorker(PipelineDecoderClient& client, AudioOutputBuffer& output, AudioDecoderFactory& factory,
                BandwidthDetector& bandwidth, HttpStreamFetcher& fetcher, std::uint64_t streamFetchLimit);

    void load(const StreamDescriptor& descriptor);
    void recover(const StreamDescriptor& descriptor);
    void stop();
    void work();
    void seek(double seconds);
    bool complete() const { return state_.load() == State::Complete; }  // any thread
    State state() const { return state_.load(); }

private:
    void setState(State s) { state_.store(s); }
    void idle();
    void fetch();
    void underrun();
    void decode();
    bool stream() const;  // output has room for 400 ms
    // The decoder-side part of seek(): byte seek, or skip after a restartAt().
    void seekDecoder(double seconds);
    DecoderStatus parseMetadata();
    void createDecoder();
    void destroyDecoder() { decoder_.reset(); }
    void dispatchStreamInfo();
    void dispatchPlaybackMetadata();

    PipelineDecoderClient& client_;
    AudioOutputBuffer& output_;
    AudioDecoderFactory& factory_;
    BandwidthDetector& bandwidth_;
    HttpStreamFetcher& fetcher_;
    const std::uint64_t streamFetchLimit_;
    std::unique_ptr<IStreamSource> stream_;
    std::unique_ptr<IAudioDecoder> decoder_;
    std::atomic<State> state_{State::Uninitialized};  // read by the audio thread
    bool streamInfoPending_ = false;
    bool metadataPending_ = false;
    double pendingSeek_ = -1.0;  // seconds; < 0 = none
    std::optional<double> restartedAt_;  // TCNG: seconds the source restarted at (restartAt)
};

}  // namespace tcng::media
