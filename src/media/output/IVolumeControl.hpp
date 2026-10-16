#pragma once
// TCNG (--volume): applies the app's volume/mute. The original receiver only forwarded volume
// to a speaker controller, which set the ALSA mixer. Implementations:
//   mixer    - the OS / device mixer: ALSA (Linux, as the original's controller: "default",
//              the first element with a playback volume; PipeWire's when ALSA's default is
//              pipewire-alsa), CoreAudio (macOS), WASAPI endpoint volume (Windows). The
//              stream stays bit-perfect.
//   software - gain applied to the samples in PortSink (not bit-perfect below 100%).
//   off      - nothing applied (the original's behaviour).

#include <memory>
#include <optional>
#include <string>

namespace tcng::media {

class PortSink;

struct VolumeState {
    double level = 0.0;  // the app's slider, 0..1
    bool mute = false;
};

class IVolumeControl {
public:
    virtual ~IVolumeControl() = default;
    virtual const char* name() const = 0;
    // Applies the slider level / mute. False if it could not be applied.
    virtual bool set(const VolumeState& state) = 0;
    // The current level read back from the device, if it has one.
    virtual std::optional<VolumeState> current() = 0;
};

enum class VolumeMode { Mixer, Software, Off };

// "mixer" | "software" | "off"; nullopt otherwise.
std::optional<VolumeMode> parseVolumeMode(const std::string& name);

// The control for `mode` on `sink`'s device. A mixer that cannot be opened logs a warning
// and returns a control that applies nothing. `alsaMixerControl` (--alsa-mixer-control, ALSA only):
// that element on the playback device's card; throws std::runtime_error if it is missing.
std::shared_ptr<IVolumeControl> makeVolumeControl(VolumeMode mode, PortSink& sink, const std::string& alsaMixerControl = "");

// Platform mixer for an output device name as PortAudio reports it; nullptr if unavailable.
std::shared_ptr<IVolumeControl> makeMixerVolume(const std::string& deviceName, const std::string& alsaMixerControl);

}  // namespace tcng::media
