#pragma once
// AudioOutputBuffer: decoded interleaved PCM between the decoder (MediaPlayer loop thread)
// and the PortAudio callback. Capacity = sizeFrames * channels samples (--audio-output-buffer is frames per
// channel).
//
// The audio callback may be reading while the ring is reallocated (setPlaybackMetadata())
// or flushed. Both take a mutex that the reader only try-locks, so the callback never
// blocks; while the buffer is being rebuilt it simply reads nothing (the player zero-fills).

#include <cstddef>
#include <functional>
#include <mutex>

#include "media/audio/Interfaces.hpp"
#include "util/RingBuffer.hpp"

namespace tcng::media {

class AudioOutputBuffer : public IAudioOutput {
public:
    using MetadataCallback = std::function<void(const AudioMetadata&)>;

    explicit AudioOutputBuffer(std::size_t sizeFrames);

    // IAudioOutput (producer side: the MediaPlayer loop thread)
    std::size_t appendInterleaved(const void* data, std::size_t frames, const AudioMetadata& md) override;
    bool isLastOutput() const override { return true; }
    std::size_t getWriteableFrames(const AudioMetadata& md) const override;
    // Reallocates (discarding contents) for the new format, then calls the metadata callback.
    void setPlaybackMetadata(const AudioMetadata& md) override;

    // Consumer side (audio callback). Returns frames copied; 0 while the buffer is being
    // reallocated or flushed.
    std::size_t readInterleavedPcmData(void* out, std::size_t frames);

    std::size_t framesAvailableForRead() const;  // producer (loop) thread only
    double getFillGrade() const;                 // any thread, non-blocking
    void flush();
    AudioMetadata getAudioMetadata() const;
    void setPlaybackMetadataCallback(MetadataCallback callback);

private:
    const std::size_t sizeFrames_;
    mutable std::mutex mutex_;  // realloc / flush vs. reader
    util::RingBuffer ring_;
    AudioMetadata metadata_;
    std::size_t frameBytes_ = 0;
    MetadataCallback callback_;
};

}  // namespace tcng::media
