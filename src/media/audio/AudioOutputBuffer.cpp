#include "media/audio/AudioOutputBuffer.hpp"

#include "util/Log.hpp"

namespace tcng::media {

AudioOutputBuffer::AudioOutputBuffer(std::size_t sizeFrames) : sizeFrames_(sizeFrames) {}

void AudioOutputBuffer::setPlaybackMetadata(const AudioMetadata& md) {
    MetadataCallback callback;
    {
        std::lock_guard lock(mutex_);
        metadata_ = md;
        const unsigned elem = bytesPerSample(md.packaging);
        const unsigned channels = md.channels ? md.channels : 2;
        frameBytes_ = static_cast<std::size_t>(elem) * channels;
        // One element per frame: same byte capacity as sizeFrames * channels samples.
        ring_.reset(sizeFrames_, frameBytes_);
        callback = callback_;
    }
    if (callback)
        callback(md);
}

std::size_t AudioOutputBuffer::appendInterleaved(const void* data, std::size_t frames, const AudioMetadata&) {
    const std::size_t written = ring_.write(data, frames);
    if (written < frames)
        log::get()->warn("AudioOutputBuffer: dropped {} frames (buffer full)", frames - written);
    return written;
}

// Producer thread only (the thread that reallocates/flushes), so no lock is needed.
std::size_t AudioOutputBuffer::getWriteableFrames(const AudioMetadata&) const {
    return ring_.freeSpace();
}

std::size_t AudioOutputBuffer::readInterleavedPcmData(void* out, std::size_t frames) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return 0;
    return ring_.read(out, frames);
}

// Producer thread only (see getWriteableFrames).
std::size_t AudioOutputBuffer::framesAvailableForRead() const {
    return ring_.occupied();
}

// May run on the audio thread: never blocks, reports empty while being rebuilt.

double AudioOutputBuffer::getFillGrade() const {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return 0.0;
    const std::size_t cap = ring_.capacity();
    return cap ? static_cast<double>(ring_.occupied()) / static_cast<double>(cap) : 0.0;
}

void AudioOutputBuffer::flush() {
    std::lock_guard lock(mutex_);
    ring_.clear();
}

AudioMetadata AudioOutputBuffer::getAudioMetadata() const {
    std::lock_guard lock(mutex_);
    return metadata_;
}

void AudioOutputBuffer::setPlaybackMetadataCallback(MetadataCallback callback) {
    std::lock_guard lock(mutex_);
    callback_ = std::move(callback);
}

}  // namespace tcng::media
