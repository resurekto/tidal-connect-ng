// TCNG (--volume): the factory, the software control and "off".

#include "util/Log.hpp"
#include "media/output/PortSink.hpp"
#include "media/output/IVolumeControl.hpp"
#include "media/output/VolumeCurve.hpp"

namespace tcng::media {

namespace {

class NoVolume : public IVolumeControl {
public:
    explicit NoVolume(const char* name) : name_(name) {}
    const char* name() const override { return name_; }
    bool set(const VolumeState&) override { return true; }
    std::optional<VolumeState> current() override { return std::nullopt; }

private:
    const char* name_;
};

class SoftwareVolume : public IVolumeControl {
public:
    explicit SoftwareVolume(PortSink& sink) : sink_(sink) {}
    const char* name() const override { return "software"; }
    bool set(const VolumeState& state) override {
        sink_.setGain(state.mute ? 0.0f : static_cast<float>(sliderToGain(state.level)));
        return true;
    }
    std::optional<VolumeState> current() override { return std::nullopt; }

private:
    PortSink& sink_;
};

}  // namespace

std::optional<VolumeMode> parseVolumeMode(const std::string& name) {
    if (name == "mixer")
        return VolumeMode::Mixer;
    if (name == "software")
        return VolumeMode::Software;
    if (name == "off")
        return VolumeMode::Off;
    return std::nullopt;
}

std::shared_ptr<IVolumeControl> makeVolumeControl(VolumeMode mode, PortSink& sink, const std::string& alsaMixerControl) {
    switch (mode) {
    case VolumeMode::Software:
        sink.enableSoftwareGain();
        log::get()->info("volume: software gain");
        return std::make_shared<SoftwareVolume>(sink);
    case VolumeMode::Mixer:
        if (auto mixer = makeMixerVolume(sink.deviceName(), alsaMixerControl))
            return mixer;
        log::get()->warn("volume: the app's volume will have no effect (try --volume software)");
        return std::make_shared<NoVolume>("mixer (unavailable)");
    case VolumeMode::Off:
        break;
    }
    return std::make_shared<NoVolume>("off");
}

}  // namespace tcng::media
