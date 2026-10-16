#pragma once
// MediaPlayer: the IMediaPlayer implementation. One loop thread runs queued control
// lambdas and ticks the current and the queued-next AudioPipeline every <= 5 ms; the
// PortAudio callback pulls PCM through onFramesRequested(). Status, progress, format,
// completion, errors and request-next go to the connected IMediaPlayer::Delegate on the
// loop thread.
//
// The process-wide instance is created once with
// createStaticMediaPlayer() and handed to engines through getMediaPlayerInstance().

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "media/IMediaPlayer.hpp"
#include "media/audio/Formats.hpp"
#include "media/output/IPcmSink.hpp"

namespace tcng::media {

class AudioPipeline;

class MediaPlayer : public IMediaPlayer, private IPcmSink::Delegate {
public:
    using Delegate = IMediaPlayer::Delegate;

    static constexpr std::uint32_t kDefaultStreamFetchLimit = 0x6400000;      // 100 MiB
    static constexpr std::uint32_t kDefaultAudioOutputBufferSize = 0x200000;  // frames per channel
    static constexpr double kBufferingResumeThreshold = 0.1;                  // fill grade
    static constexpr std::chrono::milliseconds kSeekQuietBuffering{2000};      // TCNG
    static constexpr std::uint64_t kProgressIntervalMs = 500;
    static constexpr std::uint64_t kRequestNextLeadMs = 4000;

    // Starts the loop thread; registers itself as the sink's delegate.
    explicit MediaPlayer(std::shared_ptr<IPcmSink> sink);
    const std::shared_ptr<IPcmSink>& sink() const { return sink_; }  // TCNG: for --volume
    ~MediaPlayer() override;
    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    // --stream-fetch-limit / --audio-output-buffer; apply to pipelines created afterwards.
    void setStreamFetchLimit(std::uint32_t bytes);
    void setAudioOutputBufferSize(std::uint32_t framesPerChannel);

    int init() override { return kSuccess; }
    int deinit() override { return kSuccess; }
    int connect(Delegate* delegate) override;
    int disconnect() override;
    int prepare(const AudioItem& item) override;
    int prepareNext(const AudioItem& item) override;
    int play() override;
    int pause() override;
    int stop() override;  // blocks until the loop thread has stopped playback
    int seekTo(std::uint64_t positionMs) override;
    int getStatus(PlayStatus& status) override;
    int interrupt(bool) override { return kSuccess; }

    std::uint64_t underrunCount() const { return underruns_.load(); }

private:
    void post(std::function<void()> fn);
    void loop_();
    void work_();
    void play_();
    void pause_();
    void stop_();
    playstate_t getState_() const { return state_.load(); }
    void setState_(playstate_t state);
    void dispatchState_();
    void dispatchProgress_();
    bool checkProgressForRequestNext_() const;
    void changeToNextAudioPipeline();
    void playbackMetadataUpdated(const AudioMetadata& md);
    void resetCounters_();
    std::uint64_t positionMs_() const;

    // IPcmSink::Delegate (audio thread)
    void onFramesRequested(void* out, unsigned long frames) override;

    std::shared_ptr<IPcmSink> sink_;
    std::atomic<Delegate*> delegate_{nullptr};
    std::uint32_t streamFetchLimit_ = kDefaultStreamFetchLimit;          // loop thread
    std::uint32_t audioOutputBufferSize_ = kDefaultAudioOutputBufferSize;

    std::atomic<playstate_t> state_{playstate_t::Idle};
    std::uint64_t seekRequestMs_ = 0;   // loop thread; 0 = none
    bool requestNextSent_ = false;
    bool nextLinked_ = false;           // gapless-next linked
    std::uint64_t lastProgressMs_ = 0;

    // Shared with the audio thread
    std::atomic<std::uint64_t> pendingSeekMs_{0};   // applied by onFramesRequested
    std::atomic<std::uint64_t> framesPlayed_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<bool> buffering_{false};
    std::chrono::steady_clock::time_point seekQuietUntil_{};  // TCNG, see work_
    std::atomic<unsigned> outputFrameBytes_{4};
    std::atomic<unsigned> outputRate_{0};

    // Pipelines. queue_ is loop-thread only; current_/next_ are also read by the audio
    // callback, which holds pipelineMutex_ while it uses them, so a pipeline is never
    // destroyed under the callback.
    std::deque<std::shared_ptr<AudioPipeline>> queue_;
    std::mutex pipelineMutex_;
    std::shared_ptr<AudioPipeline> current_;
    std::shared_ptr<AudioPipeline> next_;
    AudioMetadata outputMetadata_;            // loop thread

    // WorkQueue
    std::mutex workMutex_;
    std::deque<std::function<void()>> work_queue_;

    std::atomic<bool> running_{true};
    std::thread thread_;
};

// The static media player slot.
std::shared_ptr<MediaPlayer>& getStaticMediaPlayer();

// TidalConnect::Init: PortSink(playbackDevice) + MediaPlayer, then the two size setters
// (each only if non-zero). Creates the instance once; later calls return it unchanged.
// Throws std::runtime_error if PortAudio cannot be initialised or has no output device.
std::shared_ptr<MediaPlayer> createStaticMediaPlayer(const std::string& playbackDevice, std::uint32_t streamFetchLimit,
                                                     std::uint32_t audioOutputBufferSize);

// getMediaPlayerInstance(): the static player (null before createStaticMediaPlayer).
std::shared_ptr<IMediaPlayer> getMediaPlayerInstance();

}  // namespace tcng::media
