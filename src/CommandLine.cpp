#include "CommandLine.hpp"

#include <filesystem>
#include <ostream>

#include <CLI/CLI.hpp>

namespace tcng {

std::string defaultControllerSocketPath() {
#ifdef _WIN32
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    return ((ec ? std::filesystem::path(".") : dir) / "tisoc-controller").string();
#else
    return "/tmp/tisoc-controller";
#endif
}

namespace {

const char* kLogLevelHelp =
    "Log level\n"
    "      0: OFF\n"
    "      1: CRITICAL\n"
    "      2: ERROR\n"
    "      3: WARNING\n"
    "      4: INFO\n"
    "      5: DEBUG\n"
    "      6: TRACE";

CLI::Validator messageRange(long lo, long hi, std::string message) {
    return CLI::Validator(
        [lo, hi, message](std::string& value) -> std::string {
            long v = 0;
            try {
                std::size_t used = 0;
                v = std::stol(value, &used);
                if (used != value.size())
                    return "Unable to convert '" + value + "' to destination type";
            } catch (...) {
                return "Unable to convert '" + value + "' to destination type";
            }
            return (v < lo || v > hi) ? message : std::string();
        },
        "", "");
}

}  // namespace

ParseResult parseCommandLine(int argc, const char* const* argv, Config& config,
                             std::ostream& out, std::ostream& err) {
    if (config.controllerSocketPath.empty())
        config.controllerSocketPath = defaultControllerSocketPath();

    CLI::App app("TIDAL Connect receiver");
    app.set_help_flag("-?,-h,--help", "display usage information");
    app.option_defaults()->always_capture_default();

    int logLevel = 0;
    app.add_option("-l,--log-level", logLevel, kLogLevelHelp)
        ->type_name("<log_level_number>")
        ->default_str("4")
        ->check(messageRange(0, 6, "Log level outside range"));
    app.add_option("--netif-for-deviceid", config.netifForDeviceId, "Network interface for device")
        ->type_name("<network_interface_name_string>");
    app.add_option("--additional-seed", config.additionalSeed, "Additional seed value for device ID")
        ->type_name("<string>");
    app.add_option("-m,--model-name", config.modelName, "Model name for mdns")
        ->type_name("<model_name_string>");
    app.add_option("-f,--friendly-name", config.friendlyName, "Friendly name for mdns")
        ->type_name("<friendly_name_string>");
    app.add_option("-p,--websocket-port", config.websocketPort, "Websocket port")
        ->type_name("<port_number>")
        ->check(messageRange(1, 65535, "invalid port value"));
    app.add_option("--controller-unix-socket-path", config.controllerSocketPath,
                   "Speaker communicator Unix socket path")
        ->type_name("<unix_socket_path>");
    app.add_option("--web-ca-path", config.webCaPath,
                   "CA bundle (PEM) for TIDAL's servers; default: the system's store")
        ->type_name("<web_ca_cert_path>");
    app.add_option("--clientid", config.clientId,
                   "TIDAL client id of the receiver the certificate belongs to (README.md, \"Client id\"); "
                   "not used with --python-tidal-path")
        ->type_name("<tidal_client_id_string>");
    app.add_option("--tc-certificate-path", config.tcCertificatePath,
                   "Device certificate PEM: certificate, chain and private key (README.md, \"Certificate\")")
        ->type_name("<tidal_certificate_string>");
    app.add_option("--playback-device", config.playbackDevice, "Name of device to use for playback")
        ->type_name("<playback_device_string>");
    app.add_option("--stream-fetch-limit", config.streamFetchLimit,
                   "Set the limit in bytes for how much of a track to fetch")
        ->type_name("<number_of_bytes>");
    app.add_option("--audio-output-buffer", config.audioOutputBuffer,
                   "Set the buffer size to be used for decoded samples")
        ->type_name("<number_of_samples>");
    auto* pythonTidalPath = app.add_option("--python-tidal-path", config.pythonTidalPath,
                                           "Path to a python-tidal checkout; tracks are then resolved through it in "
                                           "the highest available quality (HI_RES_LOSSLESS)")
                                ->type_name("<python_tidal_path>");
    auto* pythonTidalSession = app.add_option("--python-tidal-session", config.pythonTidalSession,
                                              "python-tidal session file (created by --python-tidal-login)")
                                   ->type_name("<session_file_path>");
    auto* pythonBin = app.add_option("--python-bin", config.pythonBin,
                                     "Python interpreter for --python-tidal-path (default: python3 or python on PATH)")
                          ->type_name("<python_executable_path>");
    app.add_option("--volume", config.volume,
                   "How the app's volume is applied\n"
                   "      mixer:    the OS/device mixer (PipeWire/ALSA, CoreAudio, WASAPI);\n"
                   "                the stream stays bit-perfect\n"
                   "      software: scale the samples (not bit-perfect below 100%)\n"
                   "      off:      not applied, only forwarded to a speaker controller")
        ->type_name("<mixer|software|off>")
        ->check(CLI::IsMember({"mixer", "software", "off"}));
    app.add_option("--alsa-mixer-control", config.alsaMixerControl,
                   "With --volume mixer, Linux only: the ALSA mixer control to set (e.g. Digital) on the "
                   "playback device's card, normally with a hw:N --playback-device that bypasses the sound server. "
                   "Without it: the first control with a playback volume on ALSA's default")
        ->type_name("<alsa_control_name>");
    auto* pythonTidalLogin = app.add_flag("--python-tidal-login", config.pythonTidalLogin,
                                          "Log in to TIDAL (PKCE) through python-tidal, write --python-tidal-session "
                                          "and exit");
    pythonTidalLogin->needs(pythonTidalSession);
    pythonTidalPath->needs(pythonTidalSession);
    pythonTidalSession->needs(pythonTidalPath);
    pythonBin->needs(pythonTidalPath);

    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp&) {
        out << app.help();
        return ParseResult::HelpShown;
    } catch (const CLI::ParseError& e) {
        err << "Error in command line: " << e.what() << std::endl;
        return ParseResult::Error;
    }

    if (app.count("--log-level"))
        config.logLevel = logLevel;
    return ParseResult::Run;
}

}  // namespace tcng
