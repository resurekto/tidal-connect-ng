#include "media/output/PortSink.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include <portaudio.h>

#include "util/Log.hpp"
#include "media/output/SoftwareGain.hpp"

namespace tcng::media {

struct PortSinkCallback {
    // paCallbackMethod: statusFlags (e.g. output underflow) are ignored.
    static int run(const void*, void* output, unsigned long frameCount, const PaStreamCallbackTimeInfo*,
                   PaStreamCallbackFlags, void* userData) {
        auto* self = static_cast<PortSink*>(userData);
        if (!self->delegate_.load())
            return paAbort;
        self->render(output, frameCount);
        return paContinue;
    }
};

PortSink::PortSink(const std::string& device) {
    if (Pa_Initialize() != paNoError)
        throw std::runtime_error("Could not initialize portaudio");
    if (!device.empty()) {
        const int count = Pa_GetDeviceCount();
        for (int i = 0; i < count; ++i) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
            if (info && info->name && device == info->name) {
                deviceIndex_ = i;
                break;
            }
        }
        if (deviceIndex_ == -1) {
            std::printf("Could not find device: '%s'. Fallback to default device\n", device.c_str());
            std::printf("Valid devices are: \n");
            for (int i = 0; i < count; ++i) {
                const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
                if (info)
                    std::printf("'%s'\n", info->name);
            }
            std::fflush(stdout);
        }
    }
    if (deviceIndex_ == -1)
        deviceIndex_ = Pa_GetDefaultOutputDevice();
    if (deviceIndex_ == paNoDevice) {
        Pa_Terminate();
        throw std::runtime_error("Could not find a default playback device");
    }
    if (const PaDeviceInfo* info = Pa_GetDeviceInfo(deviceIndex_)) {
        deviceName_ = info->name ? info->name : "";
        log::get()->info("PortSink: using playback device '{}'", deviceName_);
    }
}

// The audio callback body. TCNG: software gain (--volume software).
void PortSink::render(void* output, unsigned long frames) {
    Delegate* delegate = delegate_.load();
    if (!widen16_) {
        delegate->onFramesRequested(output, frames);
        if (softwareGain_)
            applyGain(output, frames * 2, type_, gain_.load());
        return;
    }
    auto* out = static_cast<std::int32_t*>(output);
    const float gain = gain_.load();
    while (frames > 0) {
        const unsigned long n = std::min(frames, kScratchFrames);
        delegate->onFramesRequested(scratch_.data(), n);
        widenWithGain(scratch_.data(), out, n * 2, gain);
        out += n * 2;
        frames -= n;
    }
}

PortSink::~PortSink() {
    closePa();
    Pa_Terminate();
}

int PortSink::open(int rate, SampleType type, bool callbackMode) {
    if (open_)
        closePa();
    PaStreamParameters out{};
    out.device = deviceIndex_;
    out.channelCount = 2;
    type_ = type;
    widen16_ = softwareGain_ && type == SampleType::Int16;
    if (widen16_) {
        scratch_.resize(kScratchFrames * 2);
        type = SampleType::Int32;
    }
    switch (type) {
    case SampleType::Int16: out.sampleFormat = paInt16; break;
    case SampleType::Int32: out.sampleFormat = paInt32; break;
    case SampleType::Float32: out.sampleFormat = paFloat32; break;
    case SampleType::Int24: out.sampleFormat = paInt24; break;
    default: return 2;
    }
    out.suggestedLatency = 0.2;
    out.hostApiSpecificStreamInfo = nullptr;
    const PaError err = Pa_OpenStream(&stream_, nullptr, &out, static_cast<double>(rate),
                                      callbackMode ? paFramesPerBufferUnspecified : 0x8000, paClipOff,
                                      callbackMode ? &PortSinkCallback::run : nullptr, this);
    if (err != paNoError) {
        log::get()->error("Could not open stream: {}", Pa_GetErrorText(err));
        stream_ = nullptr;
        return 1;
    }
    open_ = true;
    return 0;
}

int PortSink::start() {
    if (!stream_)
        return 3;
    const PaError err = Pa_StartStream(stream_);
    if (err != paNoError) {
        log::get()->error("Could not start stream: {}", Pa_GetErrorText(err));
        return 3;
    }
    return 0;
}

int PortSink::stop() {
    if (stream_ && Pa_IsStreamStopped(stream_) == 0)
        Pa_StopStream(stream_);
    return 0;
}

void PortSink::closePa() {
    if (!stream_)
        return;
    stop();
    Pa_CloseStream(stream_);
    stream_ = nullptr;
    open_ = false;
}

int PortSink::close() {
    closePa();
    return 0;
}

long PortSink::writeFrames(const void* data, unsigned long frames) {
    return stream_ ? Pa_WriteStream(stream_, data, frames) : -1;
}

long PortSink::getFramesAvailable() {
    if (!stream_)
        return 0;
    const long n = Pa_GetStreamWriteAvailable(stream_);
    return n > 0 ? n : 0;
}

}  // namespace tcng::media
