// TCNG (--volume mixer) on Windows: the endpoint volume (IAudioEndpointVolume) of the render
// endpoint whose friendly name matches PortAudio's device name (a prefix match: MME truncates
// names), else the default render endpoint. Windows' scalar is already an audio taper, so the
// app's slider is passed through. Hardware volume where the endpoint supports it; otherwise
// the audio engine applies it.

#include <initguid.h>
#include <windows.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

#include <memory>
#include <string>

#include "util/Log.hpp"
#include "media/output/IVolumeControl.hpp"

namespace tcng::media {

namespace {

std::string narrow(const wchar_t* w) {
    if (!w)
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? static_cast<std::size_t>(n - 1) : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p)
            p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// COM on the calling thread for the duration of one call.
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }
};

std::string friendlyName(IMMDevice* device) {
    Com<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props)))
        return {};
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
        name = narrow(v.pwszVal);
    PropVariantClear(&v);
    return name;
}

// The endpoint volume for `name` (or the default endpoint), or null.
bool openEndpoint(const std::string& name, Com<IAudioEndpointVolume>& out, std::string* chosen = nullptr) {
    Com<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))))
        return false;
    Com<IMMDevice> device;
    Com<IMMDeviceCollection> all;
    UINT count = 0;
    if (!name.empty() && SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all)) &&
        SUCCEEDED(all->GetCount(&count))) {
        for (UINT i = 0; i < count && !device; ++i) {
            Com<IMMDevice> d;
            if (SUCCEEDED(all->Item(i, &d))) {
                const std::string n = friendlyName(d.p);
                if (!n.empty() && (n == name || n.rfind(name, 0) == 0 || name.rfind(n, 0) == 0)) {
                    device.p = d.p;
                    d.p = nullptr;
                }
            }
        }
    }
    if (!device && FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
        return false;
    if (chosen)
        *chosen = friendlyName(device.p);
    return SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(&out)));
}

class WasapiVolume : public IVolumeControl {
public:
    explicit WasapiVolume(std::string device) : device_(std::move(device)) {}
    const char* name() const override { return "mixer (WASAPI)"; }

    bool set(const VolumeState& state) override {
        ComScope com;
        Com<IAudioEndpointVolume> v;
        if (!openEndpoint(device_, v))
            return false;
        const bool ok = SUCCEEDED(v->SetMasterVolumeLevelScalar(static_cast<float>(state.level), nullptr));
        v->SetMute(state.mute ? TRUE : FALSE, nullptr);
        return ok;
    }

    std::optional<VolumeState> current() override {
        ComScope com;
        Com<IAudioEndpointVolume> v;
        float level = 0;
        BOOL mute = FALSE;
        if (!openEndpoint(device_, v) || FAILED(v->GetMasterVolumeLevelScalar(&level)))
            return std::nullopt;
        v->GetMute(&mute);
        return VolumeState{level, mute != FALSE};
    }

private:
    const std::string device_;
};

}  // namespace

std::shared_ptr<IVolumeControl> makeMixerVolume(const std::string& deviceName, const std::string& control) {
    if (!control.empty())
        log::get()->warn("--alsa-mixer-control is ALSA-only; ignored on Windows");
    ComScope com;
    Com<IAudioEndpointVolume> v;
    std::string chosen;
    if (!openEndpoint(deviceName, v, &chosen)) {
        log::get()->warn("volume: no WASAPI render endpoint");
        return nullptr;
    }
    log::get()->info("volume: WASAPI endpoint '{}'", chosen);
    return std::make_shared<WasapiVolume>(deviceName);
}

}  // namespace tcng::media
