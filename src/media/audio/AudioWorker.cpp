#include "media/audio/AudioWorker.hpp"

#include <algorithm>

#include "util/Log.hpp"

namespace tcng::media {

namespace {
constexpr double kDecodeChunkSeconds = 0.2;  // decode 200 ms per tick, need room for 2x
}

AudioWorker::AudioWorker(PipelineDecoderClient& client, AudioOutputBuffer& output, AudioDecoderFactory& factory,
                         BandwidthDetector& bandwidth, HttpStreamFetcher& fetcher, std::uint64_t streamFetchLimit)
    : client_(client),
      output_(output),
      factory_(factory),
      bandwidth_(bandwidth),
      fetcher_(fetcher),
      streamFetchLimit_(streamFetchLimit),
      stream_(makeStreamSource(StreamDescriptor{}, bandwidth, streamFetchLimit)) {}

void AudioWorker::load(const StreamDescriptor& descriptor) {
    log::get()->debug("AudioWorker::load url={} format={} segments={}", descriptor.url, descriptor.format,
                    descriptor.segments.size());
    destroyDecoder();  // it reads from the old source
    pendingSeek_ = -1.0;
    restartedAt_.reset();
    streamInfoPending_ = true;
    metadataPending_ = true;
    stream_ = makeStreamSource(descriptor, bandwidth_, streamFetchLimit_);
    stream_->load(descriptor);
    setState(State::Fetch);
}

void AudioWorker::recover(const StreamDescriptor& descriptor) {
    fetcher_.abort();
    stream_->load(descriptor);
    setState(State::Idle);
}

void AudioWorker::stop() {
    fetcher_.abort();
    setState(State::Stopped);
}

void AudioWorker::work() {
    switch (state_.load()) {
    case State::Idle: idle(); break;
    case State::Fetch: fetch(); break;
    case State::Decode: decode(); break;
    case State::Underrun: underrun(); break;
    default: break;
    }
}

void AudioWorker::fetch() {
    if (stream_->complete() || stream_->endOfStream()) {
        setState(State::Idle);
        return;
    }
    bandwidth_.setFileSize(stream_->size());
    const auto request = stream_->nextFetch();
    if (!request) {
        setState(State::Idle);
        return;
    }
    if (request->range)
        log::get()->trace("AudioWorker::fetch range {}-{}", request->range->begin, request->range->end);
    else
        log::get()->trace("AudioWorker::fetch {}", request->url);
    fetcher_.fetch(request->url, request->range);
    setState(State::Underrun);
}

void AudioWorker::underrun() {
    switch (fetcher_.state()) {
    case HttpStreamFetcher::State::Fetching:
        return;
    case HttpStreamFetcher::State::Done:
        stream_->append(fetcher_.chunkDescriptor());
        setState(State::Idle);
        return;
    case HttpStreamFetcher::State::MaxAttemptsReached: {
        const auto chunk = fetcher_.chunkDescriptor();
        log::get()->error("AudioWorker: max connections reached");
        client_.onMaxConnectionsReached({stream_->url(), chunk.httpStatus, chunk.statusMessage});
        setState(State::Stopped);
        return;
    }
    case HttpStreamFetcher::State::Aborted:
        return;
    default:  // Idle/Timeout/Error: back to idle, which re-fetches (the retry)
        setState(State::Idle);
        return;
    }
}

bool AudioWorker::stream() const {
    if (!decoder_ || !decoder_->metadata().valid())
        return true;
    const auto& md = decoder_->metadata();
    const auto needed = static_cast<std::size_t>(md.sampleRatePlayback * kDecodeChunkSeconds) * 2;
    return output_.getWriteableFrames(md) >= needed;
}

void AudioWorker::createDecoder() {
    stream_->seek(0);
    if (stream_->availableBytes() == 0) {
        log::get()->error("AudioWorker::createDecoder - no data available to create a decoder");
        return;
    }
    decoder_ = factory_.create(*stream_, output_);
}

void AudioWorker::dispatchStreamInfo() {
    const auto& md = decoder_->metadata();
    if (!md.valid())
        return;
    const double hint = stream_->durationHint();  // TCNG: segmented streams know it up front
    const double duration = hint > 0.0 ? hint : static_cast<double>(decoder_->totalFrames()) / md.sampleRatePlayback;
    client_.onDuration(duration);
    client_.onStreamFormat(stream_->format());
    bandwidth_.setDuration(duration);
    streamInfoPending_ = false;
}

void AudioWorker::dispatchPlaybackMetadata() {
    const auto& md = decoder_->metadata();
    if (!md.valid())
        return;
    log::get()->debug("AudioWorker: playback metadata channels={} bitsPerSample={} sampleRate={}", md.channels,
                    md.bitsPerSample, md.sampleRatePlayback);
    output_.setPlaybackMetadata(md);
    metadataPending_ = false;
}

DecoderStatus AudioWorker::parseMetadata() {
    if (decoder_->metadata().valid() && !streamInfoPending_ && !metadataPending_)
        return DecoderStatus::Success;
    const DecoderStatus status = decoder_->parseMetadata();
    log::get()->trace("AudioWorker::parseMetadata status={}", toString(status));
    if (status == DecoderStatus::Success) {
        if (streamInfoPending_)
            dispatchStreamInfo();
        if (metadataPending_)
            dispatchPlaybackMetadata();
    } else if (status == DecoderStatus::Underrun) {
        stream_->seek(0);
        setState(State::Fetch);
    } else if (isError(status)) {
        log::get()->error("AudioWorker: corrupted metadata/stream while parsing metadata");
        client_.onDecoderError(status);
        setState(State::Stopped);
    }
    return status;
}

void AudioWorker::seek(double seconds) {
    // TCNG: a segmented source cannot map the time to bytes yet; it restarts at the segment
    // holding `seconds` and a fresh decoder skips the rest (seekDecoder).
    if (const auto start = stream_->restartAt(seconds)) {
        fetcher_.abort();
        destroyDecoder();
        restartedAt_ = start;
        pendingSeek_ = seconds;
        setState(State::Fetch);
        return;
    }
    seekDecoder(seconds);
}

void AudioWorker::seekDecoder(double seconds) {
    if (!decoder_) {
        stream_->seek(0);
        if (restartedAt_ && stream_->availableBytes() == 0) {
            pendingSeek_ = seconds;  // the restarted source has nothing yet
            setState(State::Fetch);
            return;
        }
        createDecoder();
        if (!decoder_) {
            log::get()->debug("AudioWorker::seek {} postponed (no decoder yet)", seconds);
            pendingSeek_ = seconds;
            return;
        }
    }
    const DecoderStatus parsed = parseMetadata();
    if (parsed == DecoderStatus::Success && restartedAt_) {
        const auto rate = decoder_->metadata().sampleRatePlayback;
        decoder_->skipFrames(static_cast<std::uint64_t>(std::max(0.0, seconds - *restartedAt_) * rate));
        restartedAt_.reset();
        pendingSeek_ = -1.0;
        setState(State::Idle);
    } else if (parsed == DecoderStatus::Success) {
        const auto frame = static_cast<std::uint64_t>(decoder_->metadata().sampleRatePlayback * seconds);
        const DecoderStatus st = decoder_->seek(frame);
        if (st == DecoderStatus::Success) {
            pendingSeek_ = -1.0;
            setState(State::Idle);
        } else if (st == DecoderStatus::Underrun) {
            pendingSeek_ = seconds;
            setState(State::Fetch);
        } else {
            pendingSeek_ = -1.0;
        }
    } else if (parsed == DecoderStatus::Underrun) {
        // Metadata not parsed yet: remember the seek.
        pendingSeek_ = seconds;
    } else {
        pendingSeek_ = -1.0;
    }
}

void AudioWorker::idle() {
    if (!stream()) {
        // TCNG: the output is full; let a segmented source download ahead meanwhile.
        if (stream_->wantsPrefetch())
            setState(State::Fetch);
        return;
    }
    if (pendingSeek_ > 0.0) {
        log::get()->debug("AudioWorker::idle - applying pending seek {}", pendingSeek_);
        seekDecoder(pendingSeek_);
        return;
    }
    if (!decoder_) {
        setState(stream_->availableBytes() != 0 ? State::Decode : State::Fetch);
        return;
    }
    if (!decoder_->ended()) {
        setState(State::Decode);
        return;
    }
    if (stream_->complete() || stream_->endOfStream()) {
        setState(State::Complete);
        return;
    }
    setState(State::Fetch);
}

void AudioWorker::decode() {
    if (!decoder_) {
        createDecoder();
        if (!decoder_) {
            log::get()->error("AudioWorker::decode - could not create a decoder");
            setState(State::Stopped);
            return;
        }
    }
    if (!stream() || decoder_->ended()) {
        setState(State::Idle);
        return;
    }
    if (parseMetadata() != DecoderStatus::Success)
        return;
    const auto frames = static_cast<unsigned>(decoder_->metadata().sampleRatePlayback * kDecodeChunkSeconds);
    const DecoderStatus st = decoder_->decode(frames);
    log::get()->trace("AudioWorker::decode frames={} status={}", frames, toString(st));
    if (isError(st)) {
        log::get()->error("AudioWorker::decode - corrupted stream, stopping");
        client_.onDecoderError(st);
        setState(State::Stopped);
    } else if (st == DecoderStatus::Complete) {
        setState(State::Idle);
    } else if (st == DecoderStatus::Underrun) {
        setState(State::Fetch);
    }
}

}  // namespace tcng::media
