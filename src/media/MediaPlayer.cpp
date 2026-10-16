#include "media/MediaPlayer.hpp"

#include <chrono>
#include <cstring>
#include <future>

#include "util/Log.hpp"
#include "media/audio/AudioPipeline.hpp"
#include "media/output/PortSink.hpp"

namespace tcng::media {

MediaPlayer::MediaPlayer(std::shared_ptr<IPcmSink> sink) : sink_(std::move(sink)) {
    sink_->setDelegate(this);
    thread_ = std::thread([this] { loop_(); });
}

MediaPlayer::~MediaPlayer() {
    running_ = false;
    if (thread_.joinable())
        thread_.join();
    sink_->setDelegate(nullptr);
}

void MediaPlayer::setStreamFetchLimit(std::uint32_t bytes) {
    post([this, bytes] { streamFetchLimit_ = bytes; });
}

void MediaPlayer::setAudioOutputBufferSize(std::uint32_t frames) {
    post([this, frames] { audioOutputBufferSize_ = frames; });
}

// --- public API: marshalled onto the loop thread ---

void MediaPlayer::post(std::function<void()> fn) {
    std::lock_guard lock(workMutex_);
    work_queue_.push_back(std::move(fn));
}

int MediaPlayer::connect(Delegate* delegate) {
    delegate_ = delegate;
    return kSuccess;
}

int MediaPlayer::disconnect() {
    delegate_ = nullptr;
    return kSuccess;
}

int MediaPlayer::prepare(const AudioItem& item) {
    post([this, item] {
        if (getState_() != playstate_t::Idle)
            stop_();
        queue_.push_back(std::make_shared<AudioPipeline>(item, streamFetchLimit_, audioOutputBufferSize_));
        setState_(playstate_t::Paused);
    });
    return kSuccess;
}

int MediaPlayer::prepareNext(const AudioItem& item) {
    post([this, item] {
        queue_.push_back(std::make_shared<AudioPipeline>(item, streamFetchLimit_, audioOutputBufferSize_));
    });
    return kSuccess;
}

int MediaPlayer::play() {
    post([this] { play_(); });
    return kSuccess;
}

int MediaPlayer::pause() {
    post([this] { pause_(); });
    return kSuccess;
}

int MediaPlayer::seekTo(std::uint64_t positionMs) {
    post([this, positionMs] { seekRequestMs_ = positionMs ? positionMs : 1; });
    return kSuccess;
}

int MediaPlayer::stop() {
    if (std::this_thread::get_id() == thread_.get_id()) {
        stop_();  // from a delegate callback on the loop thread: waiting would deadlock
        return kSuccess;
    }
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    post([this, done] {
        stop_();
        done->set_value();
    });
    future.wait();
    return kSuccess;
}

int MediaPlayer::getStatus(PlayStatus& status) {
    status.state = getState_();
    return kSuccess;
}

// --- loop thread -------------------------------------------------------------------

void MediaPlayer::loop_() {
    while (running_) {
        do {
            work_();
        } while ([this] {
            std::lock_guard lock(workMutex_);
            return !work_queue_.empty();
        }());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stop_();
}

void MediaPlayer::setState_(playstate_t state) {
    if (getState_() == state)
        return;
    state_ = state;
    dispatchState_();
}

void MediaPlayer::dispatchState_() {
    Delegate* d = delegate_;
    if (!d)
        return;
    const PlayStatus status{getState_()};
    if (current_)
        d->onStatusUpdated(status, current_->getAudioItem());
    else
        d->onStatusUpdated(status, AudioItem{});
}

std::uint64_t MediaPlayer::positionMs_() const {
    const unsigned rate = outputRate_;
    return rate ? framesPlayed_.load() * 1000 / rate : 0;
}

void MediaPlayer::dispatchProgress_() {
    if (!current_)
        return;
    const double duration = current_->getDuration();
    if (duration == 0.0)
        return;
    const std::uint64_t pos = positionMs_();
    if (pos < lastProgressMs_ + kProgressIntervalMs)
        return;
    if (Delegate* d = delegate_)
        d->onProgressUpdated(pos, static_cast<std::uint64_t>(duration * 1000.0));
    lastProgressMs_ = pos;
}

bool MediaPlayer::checkProgressForRequestNext_() const {
    if (!current_)
        return false;
    const double duration = current_->getDuration();
    return duration > 0.0 && duration * 1000.0 < static_cast<double>(positionMs_() + kRequestNextLeadMs);
}

void MediaPlayer::resetCounters_() {
    pendingSeekMs_ = 0;
    framesPlayed_ = 0;
    lastProgressMs_ = 0;
    underruns_ = 0;
}

void MediaPlayer::changeToNextAudioPipeline() {
    resetCounters_();
    if (!nextLinked_)
        sink_->close();
    std::shared_ptr<AudioPipeline> retired;
    std::shared_ptr<AudioPipeline> retiredNext;
    {
        std::lock_guard lock(pipelineMutex_);
        retired = std::move(current_);
        retiredNext = std::move(next_);
        current_.reset();
        next_.reset();
        if (!queue_.empty()) {
            current_ = queue_.front();
            queue_.pop_front();
        }
    }
    retired.reset();  // destroyed here, on the loop thread, never under the audio callback
    retiredNext.reset();
    if (current_) {
        const AudioMetadata md = current_->getAudioMetadata();
        if (md.valid())
            playbackMetadataUpdated(md);
        else
            current_->setPlaybackMetadataCallback([this](const AudioMetadata& m) { playbackMetadataUpdated(m); });
    }
    requestNextSent_ = false;
    nextLinked_ = false;
}

// playbackMetadataUpdated: the format-change / sink (re)open point.
void MediaPlayer::playbackMetadataUpdated(const AudioMetadata& md) {
    if (Delegate* d = delegate_; d && current_) {
        const AudioItem& item = current_->getAudioItem();
        AudioFormat f;
        f.mediaformat = item.mediaformat;
        f.audioCodec = item.audioCodec;
        f.streamType = item.streamType;
        f.audioChannels = static_cast<int>(md.channels);
        f.audioBitPerSample = static_cast<int>(md.bitsPerSample);
        f.audioSamplingRate = static_cast<int>(md.sampleRateOriginal ? md.sampleRateOriginal : md.sampleRatePlayback);
        f.audioBitrate = f.audioSamplingRate * f.audioBitPerSample * f.audioChannels;
        d->onAudioFormatUpdated(f);
    }
    if (md != outputMetadata_)
        sink_->close();
    if (!sink_->isOpen()) {
        const SampleType type = md.bitsPerSample == 32   ? SampleType::Int32
                                : md.bitsPerSample == 24 ? SampleType::Int24
                                                         : SampleType::Int16;
        {
            std::lock_guard lock(pipelineMutex_);
            outputMetadata_ = md;
            outputFrameBytes_ = md.frameBytes() ? md.frameBytes() : 4;
            outputRate_ = md.sampleRatePlayback;
        }
        if (sink_->open(static_cast<int>(md.sampleRatePlayback), type, true) != 0)
            log::get()->error("MediaPlayer: could not open the audio output at {} Hz", md.sampleRatePlayback);
        else if (getState_() == playstate_t::Playing || getState_() == playstate_t::Buffering)
            sink_->start();
    }
    // Only resume PLAYING if we were playing: a format change during a pause stays paused.
    if (getState_() == playstate_t::Playing || getState_() == playstate_t::Buffering)
        setState_(playstate_t::Playing);
}

void MediaPlayer::play_() {
    const auto st = getState_();
    if (st == playstate_t::Playing || st == playstate_t::Buffering)
        return;
    if (!current_ && queue_.empty())
        return;
    setState_(playstate_t::Buffering);
    if (sink_->isOpen())
        sink_->start();
    dispatchProgress_();
}

void MediaPlayer::pause_() {
    if (getState_() == playstate_t::Paused)
        return;
    setState_(playstate_t::Paused);
    dispatchProgress_();
    sink_->pause();
}

void MediaPlayer::stop_() {
    sink_->close();
    std::shared_ptr<AudioPipeline> retired;
    std::shared_ptr<AudioPipeline> retiredNext;
    std::deque<std::shared_ptr<AudioPipeline>> retiredQueue;
    {
        std::lock_guard lock(pipelineMutex_);
        retired = std::move(current_);
        retiredNext = std::move(next_);
        current_.reset();
        next_.reset();
        outputMetadata_.clear();
        outputFrameBytes_ = 4;
        outputRate_ = 0;
    }
    retiredQueue.swap(queue_);
    retired.reset();
    retiredNext.reset();
    retiredQueue.clear();
    seekRequestMs_ = 0;
    requestNextSent_ = false;
    nextLinked_ = false;
    buffering_ = false;
    setState_(playstate_t::Idle);
    resetCounters_();
}

// work_: one tick.
void MediaPlayer::work_() {
    std::deque<std::function<void()>> jobs;
    {
        std::lock_guard lock(workMutex_);
        jobs.swap(work_queue_);
    }
    for (auto& job : jobs)
        job();

    const auto st = getState_();
    if (st != playstate_t::Playing && st != playstate_t::Buffering)
        return;
    if (!current_) {
        if (queue_.empty())
            return;
        changeToNextAudioPipeline();
        dispatchState_();
        if (!current_)
            return;
    }

    if (seekRequestMs_ != 0) {
        const std::uint64_t ms = seekRequestMs_;
        seekRequestMs_ = 0;
        current_->seek(static_cast<double>(ms) / 1000.0);
        framesPlayed_ = 0;
        pendingSeekMs_ = ms;
        // Report the target right away, so a status update sent while rebuffering (e.g.
        // BUFFERING) already carries it and the app's progress bar doesn't jump back.
        lastProgressMs_ = ms;
        // TCNG: segmented streams (--python-tidal-path) reload a segment after a seek, and the TIDAL
        // app shows the pre-seek position while BUFFERING is reported: keep PLAYING meanwhile.
        if (getState_() == playstate_t::Playing && !current_->getAudioItem().segments.empty())
            seekQuietUntil_ = std::chrono::steady_clock::now() + kSeekQuietBuffering;
        if (const double duration = current_->getDuration(); duration > 0.0)
            if (Delegate* d = delegate_)
                d->onProgressUpdated(ms, static_cast<std::uint64_t>(duration * 1000.0));
    }

    if (checkProgressForRequestNext_() && !requestNextSent_) {
        requestNextSent_ = true;
        if (Delegate* d = delegate_)
            d->onRequestNextMedia();
    }

    current_->work();
    if (!queue_.empty()) {
        const auto& front = queue_.front();
        front->work();  // the next track is fetched and decoded while the current one plays
        if (!nextLinked_) {
            const AudioMetadata md = front->getAudioMetadata();
            if (md.valid() && md == outputMetadata_) {
                std::lock_guard lock(pipelineMutex_);
                next_ = front;
                nextLinked_ = true;
            }
        }
    }

    if (buffering_ && getState_() == playstate_t::Playing && std::chrono::steady_clock::now() >= seekQuietUntil_)
        setState_(playstate_t::Buffering);
    else if (!buffering_ && getState_() == playstate_t::Buffering)
        setState_(playstate_t::Playing);

    if (isError(current_->decoderStatus())) {
        if (Delegate* d = delegate_)
            d->onPlaybackError({track_status::kDecoderError, 0, ""});
        changeToNextAudioPipeline();
    } else if (current_->maxConnectionsReached()) {
        const auto& mc = current_->maxConnectionsStatus();
        if (Delegate* d = delegate_)
            d->onPlaybackError({track_status::kMaxConnections, static_cast<int>(mc.status), mc.message});
        changeToNextAudioPipeline();
    } else if (current_->complete() && current_->drained()) {
        const unsigned rate = outputRate_;
        framesPlayed_ = static_cast<std::uint64_t>(current_->getDuration() * rate);
        dispatchProgress_();
        if (Delegate* d = delegate_)
            d->onPlaybackCompleted(!queue_.empty());
        changeToNextAudioPipeline();
        if (!current_)
            setState_(playstate_t::Idle);
    } else {
        dispatchProgress_();
    }
}

// onFramesRequested, on the PortAudio callback thread.
void MediaPlayer::onFramesRequested(void* out, unsigned long frames) {
    std::lock_guard lock(pipelineMutex_);
    const unsigned frameBytes = outputFrameBytes_;
    auto* dst = static_cast<std::uint8_t*>(out);
    std::size_t got = 0;
    if (!current_) {
        underruns_++;
        buffering_ = true;
    } else if (buffering_ && current_->getFillGrade() <= kBufferingResumeThreshold && !current_->complete()) {
        underruns_++;
    } else {
        got = current_->readInterleavedPcmData(dst, frames);
        if (got < frames) {
            if (current_->complete() && next_)  // gapless hand-over
                got += next_->readInterleavedPcmData(dst + got * frameBytes, frames - got);
            if (got < frames) {
                underruns_++;
                buffering_ = true;
            }
        } else {
            buffering_ = false;
        }
    }
    if (got < frames)
        std::memset(dst + got * frameBytes, 0, (frames - got) * frameBytes);
    if (const std::uint64_t seekMs = pendingSeekMs_.exchange(0); seekMs != 0)
        framesPlayed_ = seekMs * outputRate_.load() / 1000;
    framesPlayed_ += got;
}

// --- static instance ------------------------------------------------------------------

std::shared_ptr<MediaPlayer>& getStaticMediaPlayer() {
    static std::shared_ptr<MediaPlayer> instance;
    return instance;
}

namespace {
std::mutex g_instanceMutex;
}

std::shared_ptr<MediaPlayer> createStaticMediaPlayer(const std::string& playbackDevice, std::uint32_t streamFetchLimit,
                                                     std::uint32_t audioOutputBufferSize) {
    std::lock_guard lock(g_instanceMutex);
    auto& instance = getStaticMediaPlayer();
    if (instance)
        return instance;
    auto sink = std::make_shared<PortSink>(playbackDevice);
    instance = std::make_shared<MediaPlayer>(sink);
    if (streamFetchLimit != 0)
        instance->setStreamFetchLimit(streamFetchLimit);
    if (audioOutputBufferSize != 0)
        instance->setAudioOutputBufferSize(audioOutputBufferSize);
    return instance;
}

std::shared_ptr<IMediaPlayer> getMediaPlayerInstance() {
    std::lock_guard lock(g_instanceMutex);
    return getStaticMediaPlayer();
}

}  // namespace tcng::media
