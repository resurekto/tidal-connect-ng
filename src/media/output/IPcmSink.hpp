#pragma once
// IPcmSink: the PCM output the MediaPlayer plays through (PortSink).

#include <cstddef>

namespace tcng::media {

// Sample types: 0 int16, 1 int32, 2 float32, 3 packed int24.
enum class SampleType : int { Int16 = 0, Int32 = 1, Float32 = 2, Int24 = 3 };

class IPcmSink {
public:
    class Delegate {
    public:
        virtual ~Delegate() = default;
        // Called on the audio callback thread: fill `frames` stereo frames into `out`.
        virtual void onFramesRequested(void* out, unsigned long frames) = 0;
    };

    virtual ~IPcmSink() = default;
    // Opens a 2-channel stream at `rate`. callbackMode: pull through the Delegate.
    // Returns 0 on success.
    virtual int open(int rate, SampleType type, bool callbackMode) = 0;
    virtual int close() = 0;
    virtual bool isOpen() const = 0;
    virtual int start() = 0;
    virtual int stop() = 0;
    virtual int pause() = 0;                                             // same as stop
    virtual long writeFrames(const void* data, unsigned long frames) = 0;  // blocking mode
    virtual long getFramesAvailable() = 0;
    virtual void setDelegate(Delegate* delegate) = 0;
};

}  // namespace tcng::media
