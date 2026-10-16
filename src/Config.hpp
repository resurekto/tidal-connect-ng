#pragma once
// Settings from the command line.
// Defaults are the original's.

#include <cstdint>
#include <optional>
#include <string>

namespace tcng {

struct Config {
    std::optional<int> logLevel;                 // --log-level 0..6; absent: info
    std::string netifForDeviceId;                // --netif-for-deviceid; empty: OS machine id
    std::string additionalSeed;                  // --additional-seed
    std::string modelName = "TidalConnect";      // --model-name
    std::string friendlyName = "TidalConnect";   // --friendly-name
    std::uint16_t websocketPort = 2019;          // --websocket-port
    std::string controllerSocketPath;            // --controller-unix-socket-path
    std::string webCaPath;                       // --web-ca-path
    std::string clientId;                        // --clientid; required without --python-tidal-path
    std::string tcCertificatePath;               // --tc-certificate-path (PEM)
    std::string playbackDevice;                  // --playback-device
    std::uint32_t streamFetchLimit = 0;          // --stream-fetch-limit, 0 = library default
    std::uint32_t audioOutputBuffer = 0;         // --audio-output-buffer, 0 = library default
    // TCNG: hi-res through python-tidal. Setting pythonTidalPath switches track resolution from
    // the built-in fetch (capped at LOSSLESS 16/44.1) to the python-tidal helper.
    std::string pythonTidalPath;                 // --python-tidal-path: python-tidal checkout
    std::string pythonTidalSession;              // --python-tidal-session: tidalapi session file
    std::string pythonBin;                       // --python-bin; empty: python3/python on PATH
    bool pythonTidalLogin = false;               // --python-tidal-login: log in, write the session file, exit
    // TCNG: how the app's volume is applied (the original only forwarded it to a speaker
    // controller): "mixer" (OS/device mixer, bit-perfect), "software", "off".
    std::string volume = "mixer";                // --volume
    std::string alsaMixerControl;                // --alsa-mixer-control: ALSA element; empty = the original's choice
};

// Platform default for the controller socket: /tmp/tisoc-controller on POSIX (as the
// original), <temp dir>\tisoc-controller on Windows.
std::string defaultControllerSocketPath();

}  // namespace tcng
