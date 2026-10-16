// --volume mixer on Linux: the ALSA mixer, as the original's speaker controller set it:
// device "default", the first element with a playback volume, mute = volume 0. TCNG also
// uses the element's mute switch for mute and level 0 (see set()), applies the perceptual
// curve (VolumeCurve.hpp) and reads the level back. With pipewire-alsa, "default" is
// PipeWire's volume for the default output.
//
// --alsa-mixer-control <name>: that element instead, on the playback device's card ("(hw:N" in
// PortAudio's device name), else on "default". Opened per call.

#include <alsa/asoundlib.h>

#include <memory>
#include <regex>
#include <stdexcept>

#include "util/Log.hpp"
#include "media/output/IVolumeControl.hpp"
#include "media/output/VolumeCurve.hpp"

namespace tcng::media {

namespace {

class Mixer {
public:
    // `control` empty: the first element with a playback volume (the original).
    Mixer(const std::string& card, const std::string& control) {
        if (snd_mixer_open(&handle_, 0) < 0) {
            handle_ = nullptr;
            return;
        }
        if (snd_mixer_attach(handle_, card.c_str()) < 0 || snd_mixer_selem_register(handle_, nullptr, nullptr) < 0 ||
            snd_mixer_load(handle_) < 0) {
            snd_mixer_close(handle_);
            handle_ = nullptr;
            return;
        }
        if (!control.empty()) {
            snd_mixer_selem_id_t* id = nullptr;
            snd_mixer_selem_id_alloca(&id);
            snd_mixer_selem_id_set_index(id, 0);
            snd_mixer_selem_id_set_name(id, control.c_str());
            if (snd_mixer_elem_t* e = snd_mixer_find_selem(handle_, id); e && snd_mixer_selem_has_playback_volume(e))
                elem_ = e;
            return;
        }
        for (snd_mixer_elem_t* e = snd_mixer_first_elem(handle_); e; e = snd_mixer_elem_next(e))
            if (snd_mixer_selem_has_playback_volume(e)) {
                elem_ = e;
                return;
            }
    }
    ~Mixer() {
        if (handle_)
            snd_mixer_close(handle_);
    }
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    snd_mixer_elem_t* elem() const { return elem_; }
    bool opened() const { return handle_ != nullptr; }

    // The playback-volume controls, for error messages.
    std::string controls() const {
        std::string out;
        for (snd_mixer_elem_t* e = handle_ ? snd_mixer_first_elem(handle_) : nullptr; e; e = snd_mixer_elem_next(e))
            if (snd_mixer_selem_has_playback_volume(e))
                out += (out.empty() ? "'" : ", '") + std::string(snd_mixer_selem_get_name(e)) + "'";
        return out.empty() ? "none" : out;
    }

    // dB range in dB, if the element has one wider than alsamixer's linear limit.
    bool dbRange(double& minDb, double& maxDb) const {
        long lo = 0, hi = 0;
        if (snd_mixer_selem_get_playback_dB_range(elem_, &lo, &hi) < 0 || hi - lo <= kMaxLinearDbScaleCenti)
            return false;
        minDb = lo / 100.0;
        maxDb = hi / 100.0;
        return true;
    }

    static constexpr long kMaxLinearDbScaleCenti = static_cast<long>(kMaxLinearDbScale * 100);

private:
    snd_mixer_t* handle_ = nullptr;
    snd_mixer_elem_t* elem_ = nullptr;
};

class AlsaMixerVolume : public IVolumeControl {
public:
    AlsaMixerVolume(std::string card, std::string control) : card_(std::move(card)), control_(std::move(control)) {}
    const char* name() const override { return "mixer (ALSA)"; }

    bool set(const VolumeState& state) override {
        Mixer m(card_, control_);
        snd_mixer_elem_t* e = m.elem();
        if (!e)
            return false;
        double minDb = 0, maxDb = 0;
        long lo = 0, hi = 0;
        snd_mixer_selem_get_playback_volume_range(e, &lo, &hi);
        // Mute or level 0: volume 0, as the original, and the element's mute switch where
        // it has one, since the bottom of many volume controls is not silence.
        const bool silent = state.mute || state.level <= 0.0;
        const double level = silent ? 0.0 : state.level;
        int rc = 0;
        if (m.dbRange(minDb, maxDb) && level > 0.0)
            rc = snd_mixer_selem_set_playback_dB_all(e, static_cast<long>(sliderToDb(level, minDb, maxDb) * 100), 1);
        else
            rc = snd_mixer_selem_set_playback_volume_all(e, lo + static_cast<long>(level * static_cast<double>(hi - lo) + 0.5));
        if (snd_mixer_selem_has_playback_switch(e))
            snd_mixer_selem_set_playback_switch_all(e, silent ? 0 : 1);
        return rc >= 0;
    }

    std::optional<VolumeState> current() override {
        Mixer m(card_, control_);
        snd_mixer_elem_t* e = m.elem();
        if (!e)
            return std::nullopt;
        const auto ch = snd_mixer_selem_is_playback_mono(e) ? SND_MIXER_SCHN_MONO : SND_MIXER_SCHN_FRONT_LEFT;
        VolumeState s;
        double minDb = 0, maxDb = 0;
        long v = 0;
        if (m.dbRange(minDb, maxDb) && snd_mixer_selem_get_playback_dB(e, ch, &v) >= 0) {
            s.level = dbToSlider(v / 100.0, minDb, maxDb);
        } else {
            long lo = 0, hi = 0;
            snd_mixer_selem_get_playback_volume_range(e, &lo, &hi);
            if (snd_mixer_selem_get_playback_volume(e, ch, &v) < 0 || hi <= lo)
                return std::nullopt;
            s.level = static_cast<double>(v - lo) / static_cast<double>(hi - lo);
        }
        int on = 1;
        if (snd_mixer_selem_has_playback_switch(e) && snd_mixer_selem_get_playback_switch(e, ch, &on) >= 0)
            s.mute = on == 0;
        return s;
    }

private:
    const std::string card_;
    const std::string control_;
};

}  // namespace

std::shared_ptr<IVolumeControl> makeMixerVolume(const std::string& deviceName, const std::string& control) {
    std::string card = "default";
    std::smatch m;
    if (!control.empty() && std::regex_search(deviceName, m, std::regex(R"(\(hw:(\d+))")))
        card = "hw:" + m[1].str();
    Mixer probe(card, control);
    if (!probe.elem()) {
        if (!control.empty())
            throw std::runtime_error("--alsa-mixer-control: no playback volume control '" + control + "' on ALSA device '" +
                                     card + "' (it has: " + probe.controls() + ")");
        log::get()->warn("volume: no ALSA mixer with a playback volume on '{}'", card);
        return nullptr;
    }
    log::get()->info("volume: ALSA mixer '{}' element '{}'", card, snd_mixer_selem_get_name(probe.elem()));
    return std::make_shared<AlsaMixerVolume>(card, control);
}

}  // namespace tcng::media
