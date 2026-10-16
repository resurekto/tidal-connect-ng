#include "TidalConnect.hpp"

#include <filesystem>
#include <stdexcept>

#include "core/ClientCommunicator.hpp"
#include "core/DeviceInfo.hpp"
#include "core/MessageCore.hpp"
#include "core/MessageIds.hpp"
#include "core/PlaybackController.hpp"
#include "core/SessionManager.hpp"
#include "core/MdnsServer.hpp"
#include "media/MediaPlayer.hpp"
#include "media/PlaybackEngineTidal.hpp"
#include "media/PythonTidalFetchTask.hpp"
#include "media/TidalUrlFetchTask.hpp"
#include "media/output/PortSink.hpp"
#include "media/output/IVolumeControl.hpp"
#include "util/HttpClient.hpp"
#include "net/Loop.hpp"
#include "queue/QueueManager.hpp"
#include "spkcon/SpeakerCommunicator.hpp"
#include "util/Log.hpp"

namespace tcng {

// The python-tidal switches checked (the session file only if `sessionMustExist`) and
// resolved into absolute paths and an interpreter.
static media::PythonTidalConfig pythonTidalConfig(const Config& config, bool sessionMustExist) {
    std::error_code ec;
    if (!std::filesystem::is_directory(config.pythonTidalPath, ec))
        throw std::runtime_error("--python-tidal-path: not a directory: " + config.pythonTidalPath);
    if (sessionMustExist && !std::filesystem::is_regular_file(config.pythonTidalSession, ec))
        throw std::runtime_error("--python-tidal-session: file not found: " + config.pythonTidalSession +
                                 " (create it with --python-tidal-login)");
    const auto python = media::PythonTidalFetchTask::resolvePython(config.pythonBin);
    if (!python)
        throw std::runtime_error(config.pythonBin.empty()
                                     ? "--python-tidal-path: no python3 or python on PATH (use --python-bin)"
                                     : "--python-bin: not an executable: " + config.pythonBin);
    media::PythonTidalConfig pc;
    pc.pythonBin = *python;
    pc.tidalPath = std::filesystem::absolute(config.pythonTidalPath, ec).string();
    pc.sessionPath = std::filesystem::absolute(config.pythonTidalSession, ec).string();
    return pc;
}

int runPythonTidalLogin(const Config& config) {
    return media::runPythonTidalLogin(pythonTidalConfig(config, false));
}

std::shared_ptr<const media::ITrackFetchTaskFactory> TidalConnect::makeFetchTaskFactory(const Config& config) {
    if (config.pythonTidalPath.empty())
        return std::make_shared<media::TidalUrlFetchTaskFactory>();
    media::PythonTidalConfig pc = pythonTidalConfig(config, true);
    log::get()->info("python-tidal: {} with {}", pc.pythonBin, pc.tidalPath);
    return std::make_shared<media::PythonTidalFetchTaskFactory>(std::move(pc));
}

TidalConnect::TidalConnect(Config config) : config_(std::move(config)) {}

TidalConnect::~TidalConnect() {
    stop();
}

void TidalConnect::start() {
    if (started_)
        return;
    if (config_.clientId.empty() && config_.pythonTidalPath.empty())
        throw std::runtime_error("--clientid must be given (see README.md, \"Client id\").");
    if (config_.tcCertificatePath.empty())
        throw std::runtime_error("--tc-certificate-path must name the device certificate PEM (see README.md, \"Certificate\").");
    if (!std::filesystem::exists(config_.tcCertificatePath))
        throw std::runtime_error("certificate file not found: " + config_.tcCertificatePath);

    auto fetchTasks = makeFetchTaskFactory(config_);

    util::HttpClient::globalInit(config_.webCaPath);
    auto player =
        media::createStaticMediaPlayer(config_.playbackDevice, config_.streamFetchLimit, config_.audioOutputBuffer);
    const auto volumeMode = media::parseVolumeMode(config_.volume).value_or(media::VolumeMode::Mixer);
    if (!config_.alsaMixerControl.empty() && volumeMode != media::VolumeMode::Mixer)
        throw std::runtime_error("--alsa-mixer-control needs --volume mixer");
    std::shared_ptr<media::IVolumeControl> volume;
    if (auto sink = std::dynamic_pointer_cast<media::PortSink>(player->sink()))
        volume = media::makeVolumeControl(volumeMode, *sink, config_.alsaMixerControl);

    loop_ = std::make_unique<net::Loop>();
    manager_ = std::make_shared<core::ServiceManager>();

    auto clientcomm = std::make_shared<core::ClientCommunicator>(
        *loop_, core::ClientCommunicator::Config{config_.websocketPort, config_.tcCertificatePath});
    auto queue = std::make_shared<queue::QueueManager>();
    auto playback = std::make_shared<core::PlaybackController>(media::getMediaPlayerInstance());
    auto session = std::make_shared<core::SessionManager>();
    auto speaker = std::make_shared<spkcon::SpeakerCommunicator>(
        *loop_, spkcon::SpeakerCommunicator::Config{config_.controllerSocketPath, volume});
    auto mdns = std::make_shared<core::MdnsServer>();

    playback->registerPlaybackEngine("tidal", std::make_shared<media::PlaybackEngineTidal>(config_.clientId, fetchTasks));

    const std::string deviceId = core::DeviceInfo::deviceId(config_.netifForDeviceId, config_.additionalSeed);
    log::get()->info("device id: {}", deviceId);
    core::MdnsServer::Config mdnsConfig;
    mdnsConfig.modelName = config_.modelName;
    mdnsConfig.friendlyName = config_.friendlyName;
    mdnsConfig.port = config_.websocketPort;
    mdnsConfig.deviceId = deviceId;
    if (!mdns->setConfig(mdnsConfig))
        throw std::runtime_error("invalid mdns configuration (names must be shorter than 256 bytes)");

    // Registration order matters: startAll() runs in reverse (mdns first, clientcomm last).
    manager_->registerService(clientcomm);
    manager_->registerService(queue);
    manager_->registerService(playback);
    manager_->registerService(session);
    manager_->registerService(speaker);
    manager_->registerService(mdns);

    loop_->start();
    manager_->start(core::service::speaker);
    manager_->startAll();
    started_ = true;
}

void TidalConnect::stop() {
    if (!started_)
        return;
    manager_->finishAll();
    manager_.reset();
    util::HttpClient::globalDeinit();
    loop_->stop();
    loop_.reset();
    started_ = false;
}

}  // namespace tcng
