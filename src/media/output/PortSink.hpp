#pragma once
// PortSink: PortAudio output. The device is chosen by exact, case-sensitive name
// (--playback-device); empty or not found = the default output device. Always 2 channels at
// the track's native rate (no resampling), suggested latency 0.2 s.
//
// TCNG (--volume software): enableSoftwareGain() before open() makes the callback scale the
// samples by setGain(); 16-bit streams are then opened as 32-bit and widened.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "media/output/IPcmSink.hpp"

typedef void PaStream;

namespace tcng::media {

class PortSink : public IPcmSink {
public:
    // Throws std::runtime_error("Could not initialize portaudio") /
    // ("Could not find a default playback device").
    explicit PortSink(const std::string& device);
    ~PortSink() override;
    PortSink(const PortSink&) = delete;
    PortSink& operator=(const PortSink&) = delete;

    int open(int rate, SampleType type, bool callbackMode) override;
    int close() override;
    bool isOpen() const override { return open_; }
    int start() override;
    int stop() override;
    int pause() override { return stop(); }
    long writeFrames(const void* data, unsigned long frames) override;
    long getFramesAvailable() override;
    void setDelegate(Delegate* delegate) override { delegate_ = delegate; }

    int deviceIndex() const { return deviceIndex_; }
    // PortAudio's name of the device in use (after the default-device fallback).
    const std::string& deviceName() const { return deviceName_; }

    void enableSoftwareGain() { softwareGain_ = true; }
    void setGain(float gain) { gain_ = gain; }  // any thread

private:
    friend struct PortSinkCallback;
    void closePa();
    void render(void* output, unsigned long frames);

    static constexpr unsigned long kScratchFrames = 4096;

    int deviceIndex_ = -1;
    std::string deviceName_;
    bool softwareGain_ = false;
    std::atomic<float> gain_{1.0f};
    SampleType type_ = SampleType::Int16;    // what the delegate writes
    bool widen16_ = false;                   // Int16 delegate, Int32 stream
    std::vector<std::int16_t> scratch_;      // widen16_ only
    PaStream* stream_ = nullptr;
    bool open_ = false;
    std::atomic<Delegate*> delegate_{nullptr};
};

}  // namespace tcng::media
