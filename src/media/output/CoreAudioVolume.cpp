// TCNG (--volume mixer) on macOS: the CoreAudio volume of the output device whose name matches
// PortAudio's (else the default output device). The main element's VolumeScalar if settable,
// else channels 1 and 2. CoreAudio's scalar is already a perceptual (fader) curve, so the
// app's slider is passed through. Mute through kAudioDevicePropertyMute where settable.

#include <CoreAudio/CoreAudio.h>

#include <memory>
#include <vector>

#include "util/Log.hpp"
#include "media/output/IVolumeControl.hpp"

namespace tcng::media {

namespace {

constexpr AudioObjectPropertyElement kMain = 0;  // kAudioObjectPropertyElementMain (macOS 12+)

AudioObjectPropertyAddress address(AudioObjectPropertySelector selector, AudioObjectPropertyElement element,
                                   AudioObjectPropertyScope scope = kAudioDevicePropertyScopeOutput) {
    return {selector, scope, element};
}

std::string deviceName(AudioObjectID id) {
    auto addr = address(kAudioObjectPropertyName, kMain, kAudioObjectPropertyScopeGlobal);
    CFStringRef name = nullptr;
    UInt32 size = sizeof(name);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &name) != noErr || !name)
        return {};
    char buf[512] = {};
    CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(name);
    return buf;
}

AudioObjectID findDevice(const std::string& name) {
    auto addr = address(kAudioHardwarePropertyDevices, kMain, kAudioObjectPropertyScopeGlobal);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) == noErr && size) {
        std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) == noErr)
            for (AudioObjectID id : ids)
                if (!name.empty() && deviceName(id) == name)
                    return id;
    }
    AudioObjectID id = kAudioObjectUnknown;
    auto def = address(kAudioHardwarePropertyDefaultOutputDevice, kMain, kAudioObjectPropertyScopeGlobal);
    size = sizeof(id);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &def, 0, nullptr, &size, &id);
    return id;
}

bool settable(AudioObjectID id, const AudioObjectPropertyAddress& addr) {
    Boolean can = false;
    return AudioObjectHasProperty(id, &addr) && AudioObjectIsPropertySettable(id, &addr, &can) == noErr && can;
}

class CoreAudioVolume : public IVolumeControl {
public:
    CoreAudioVolume(AudioObjectID id, std::vector<AudioObjectPropertyElement> elements)
        : id_(id), elements_(std::move(elements)) {}
    const char* name() const override { return "mixer (CoreAudio)"; }

    bool set(const VolumeState& state) override {
        auto muteAddr = address(kAudioDevicePropertyMute, kMain);
        const bool canMute = settable(id_, muteAddr);
        const Float32 level = static_cast<Float32>(state.mute && !canMute ? 0.0 : state.level);
        bool ok = true;
        for (auto element : elements_) {
            auto addr = address(kAudioDevicePropertyVolumeScalar, element);
            ok &= AudioObjectSetPropertyData(id_, &addr, 0, nullptr, sizeof(level), &level) == noErr;
        }
        if (canMute) {
            const UInt32 mute = state.mute ? 1 : 0;
            AudioObjectSetPropertyData(id_, &muteAddr, 0, nullptr, sizeof(mute), &mute);
        }
        return ok;
    }

    std::optional<VolumeState> current() override {
        auto addr = address(kAudioDevicePropertyVolumeScalar, elements_.front());
        Float32 level = 0;
        UInt32 size = sizeof(level);
        if (AudioObjectGetPropertyData(id_, &addr, 0, nullptr, &size, &level) != noErr)
            return std::nullopt;
        VolumeState s{level, false};
        auto muteAddr = address(kAudioDevicePropertyMute, kMain);
        UInt32 mute = 0;
        size = sizeof(mute);
        if (AudioObjectHasProperty(id_, &muteAddr) &&
            AudioObjectGetPropertyData(id_, &muteAddr, 0, nullptr, &size, &mute) == noErr)
            s.mute = mute != 0;
        return s;
    }

private:
    const AudioObjectID id_;
    const std::vector<AudioObjectPropertyElement> elements_;
};

}  // namespace

std::shared_ptr<IVolumeControl> makeMixerVolume(const std::string& name, const std::string& control) {
    if (!control.empty())
        log::get()->warn("--alsa-mixer-control is ALSA-only; ignored on macOS");
    const AudioObjectID id = findDevice(name);
    if (id == kAudioObjectUnknown) {
        log::get()->warn("volume: no CoreAudio output device");
        return nullptr;
    }
    std::vector<AudioObjectPropertyElement> elements;
    if (settable(id, address(kAudioDevicePropertyVolumeScalar, kMain)))
        elements = {kMain};
    else if (settable(id, address(kAudioDevicePropertyVolumeScalar, 1)) &&
             settable(id, address(kAudioDevicePropertyVolumeScalar, 2)))
        elements = {1, 2};
    if (elements.empty()) {
        log::get()->warn("volume: '{}' has no settable volume (use --volume software)", deviceName(id));
        return nullptr;
    }
    log::get()->info("volume: CoreAudio device '{}'", deviceName(id));
    return std::make_shared<CoreAudioVolume>(id, std::move(elements));
}

}  // namespace tcng::media
