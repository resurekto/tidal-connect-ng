#include "spkcon/SpeakerCommunicator.hpp"

#include <map>

#include "util/JsonUtil.hpp"
#include "core/MessageIds.hpp"
#include "util/Log.hpp"

namespace tcng::spkcon {

using core::Json;
namespace msg = core::msg;
namespace svc = core::service;

namespace {

constexpr double kVolumeLevelMin = 0.0;
constexpr double kVolumeLevelMax = 1.0;
constexpr double kDefaultVolumeLevel = 0.2;
constexpr int kSessionCasting = 2;

constexpr int kResultSuccess = 0;
constexpr int kResultInvalidParameter = 1001;

struct CommandItem {
    const char* service;
    std::uint32_t id;
};

// Controller commands routed to other services.
const std::map<std::string, CommandItem>& commandTable() {
    static const std::map<std::string, CommandItem> table{
        {"grantResources", {svc::session, msg::SessionGrantResources}},
        {"revokeResources", {svc::session, msg::SessionRevokeResources}},
        {"previous", {svc::queue, msg::QueuePrevious}},
        {"next", {svc::queue, msg::QueueNext}},
        {"setRepeatMode", {svc::queue, msg::QueueSetRepeatMode}},
        {"setShuffle", {svc::queue, msg::QueueSetShuffle}},
        {"play", {svc::playback, msg::PlaybackPlay}},
        {"pause", {svc::playback, msg::PlaybackPause}},
        {"stop", {svc::playback, msg::PlaybackStop}},
        {"interrupt", {svc::playback, msg::PlaybackInterruptSpeaker}},
        {"changeFriendlyName", {svc::mdns, msg::MdnsChangeFriendlyName}},
        {"notifyDeviceStatusChanged", {svc::clientcomm, msg::SendPacket}},
    };
    return table;
}

}  // namespace

SpeakerCommunicator::SpeakerCommunicator(net::Loop& loop, Config config)
    : Service(svc::speaker), loop_(loop), config_(std::move(config)) {}

SpeakerCommunicator::~SpeakerCommunicator() {
    finish();
}

bool SpeakerCommunicator::start(const std::shared_ptr<core::IServiceManager>& manager) {
    manager_ = manager;
    return Service::start(manager);
}

void SpeakerCommunicator::start() {
    auto logger = log::get();
    logger->info("## SpeakerCommunicator Config ##");
    logger->info(" controller-unix-socket-path : \"{}\"", config_.controllerUnixSocketPath);
    // TCNG: report the device's level to the app; a control that cannot be read (software)
    // starts at the level the app is shown.
    if (config_.volume) {
        logger->info(" volume : {}", config_.volume->name());
        if (config_.volume->current())
            readVolume();
        else
            applyVolume();
    }
    if (!server_) {
        server_ = std::make_unique<SpkconServer>(loop_, *this);
        if (!server_->start(config_.controllerUnixSocketPath))
            logger->critical("[ASSERT] start(ret)");
    }
    Service::start();
}

void SpeakerCommunicator::finish() {
    if (server_) {
        server_->stop();
        server_.reset();
    }
    Service::finish();
}

// --- SpkconServer::Delegate (socket thread) -----------------------------------

void SpeakerCommunicator::onOpen() {
    log::get()->info("[onOpen] Connected.");
}

void SpeakerCommunicator::onClose(Reason reason) {
    log::get()->info("[onClose] Disconnected. reason:{}", static_cast<int>(reason));
}

void SpeakerCommunicator::onReceivePacket(const std::string& packet) {
    log::get()->debug("[onReceivePacket] packet:{}", packet);
    Json j = Json::parse(packet, nullptr, false);
    if (j.is_discarded()) {
        log::get()->error("[ASSERT] onReceivePacket(parse_err.empty() == true)");
        j = Json();
    }
    postMessage(msg::SpeakerRequestFromController, j);
}

// --- Messages -----------------------------------------------------------------

void SpeakerCommunicator::handleMessage(std::uint32_t id, const Json& j) {
    switch (id) {
    case msg::SpeakerRequestFromController: procRequestFromController(j); break;
    case msg::SpeakerSetVolume: procSetVolume(j); break;
    case msg::SpeakerSetMute: procSetMute(j); break;
    case msg::SpeakerRequestResources: procRequestResources(j); break;
    case msg::SpeakerReleaseResources: procReleaseResources(j); break;
    case msg::SpeakerNotifyAudioFormatUpdate:
        if (server_ && server_->isConnected())
            sendToController(j);
        break;
    case msg::SendPacket: procSendPacket(j); break;
    case msg::NotifySessionState: procNotifySessionState(j); break;
    case msg::RequestStatusNotification: appNotifyDeviceStatusChanged(0); break;
    default: break;  // ignored
    }
}

void SpeakerCommunicator::procRequestFromController(const Json& j) {
    const std::string cmd = jv::string(j, "command");
    if (cmd == "setVolumeMute")
        handleCmdSetVolumeMute(j);
    else if (cmd == "startService")
        handleCmdStartService(j);
    else if (cmd == "stopService")
        handleCmdStopService();
    else
        handleCmdCommon(cmd, j);
}

void SpeakerCommunicator::handleCmdSetVolumeMute(const Json& j) {
    const int requestId = jv::integer(j, "requestId");
    const Json& volume = jv::at(j, "volume");
    const double level = jv::number(volume, "level");
    if (!(level >= kVolumeLevelMin && level <= kVolumeLevelMax)) {
        log::get()->error("[ASSERT] handleCmdSetVolumeMute(level >= kVolumeLevelMin && level <= kVolumeLevelMax)");
        controllerNotifyRequestResult(requestId, kResultInvalidParameter);
        return;
    }
    volume_.level = level;
    volume_.mute = jv::boolean(volume, "mute");
    log::get()->info("Volume:{} mute:{}", volume_.level, volume_.mute);
    if (sessionState_ == kSessionCasting)
        appNotifyDeviceStatusChanged(0);
    controllerNotifyRequestResult(requestId, kResultSuccess);
}

void SpeakerCommunicator::handleCmdStartService(const Json& j) {
    auto manager = manager_.lock();
    if (!manager) {
        log::get()->error("autoStart is true");
        return;
    }
    log::get()->info("startAll");
    manager->startAll();
    if (!jv::at(j, "modelName").is_null()) {
        const std::string modelName = jv::string(j, "modelName");
        const std::string friendlyName = jv::string(j, "friendlyName");
        const Json& volume = jv::at(j, "volume");
        const double level = jv::number(volume, "level");
        if (!(level >= kVolumeLevelMin && level <= kVolumeLevelMax)) {
            log::get()->error("[ASSERT] handleCmdStartService(level >= kVolumeLevelMin && level <= kVolumeLevelMax)");
        } else {
            volume_.level = level;
            volume_.mute = jv::boolean(volume, "mute");
        }
        mdnsSetModelInfo(modelName, friendlyName);
    }
    mdnsStartServer();
    controllerNotifyServiceStateChanged(1);
    if (sessionState_ == kSessionCasting)
        appNotifyDeviceStatusChanged(0);
}

void SpeakerCommunicator::handleCmdStopService() {
    if (auto manager = manager_.lock()) {
        log::get()->info("finishAll");
        mdnsStopServer();
        manager->finishAllOthers(getName());
        controllerNotifyServiceStateChanged(0);
    }
    volume_.mute = false;
    volume_.level = kDefaultVolumeLevel;
}

void SpeakerCommunicator::handleCmdCommon(const std::string& cmd, const Json& j) {
    const auto& table = commandTable();
    auto it = table.find(cmd);
    if (it == table.end()) {
        log::get()->error("Invalid command received. command={}", cmd);
        return;
    }
    sendMessage(it->second.service, it->second.id, Json{{"packet", j}, {"socketId", 0}});
}

void SpeakerCommunicator::procSetVolume(const Json& j) {
    const Json& packet = jv::at(j, "packet");
    const int socketId = jv::integer(j, "socketId");
    const int requestId = jv::integer(packet, "requestId");
    volume_.level = jv::number(packet, "level");
    if (volume_.mute && volume_.level > 0)
        volume_.mute = false;
    applyVolume();
    if (sessionState_ == kSessionCasting)
        appNotifyDeviceStatusChanged(socketId);
    if (server_ && server_->isConnected())
        controllerSetVolumeMute();
    appNotifyRequestResult(socketId, requestId, kResultSuccess);
}

void SpeakerCommunicator::procSetMute(const Json& j) {
    const Json& packet = jv::at(j, "packet");
    const int socketId = jv::integer(j, "socketId");
    const int requestId = jv::integer(packet, "requestId");
    volume_.mute = jv::boolean(packet, "mute");
    applyVolume();
    if (sessionState_ == kSessionCasting)
        appNotifyDeviceStatusChanged(socketId);
    if (server_ && server_->isConnected())
        controllerSetVolumeMute();
    appNotifyRequestResult(socketId, requestId, kResultSuccess);
}

void SpeakerCommunicator::procRequestResources(const Json& j) {
    if (server_ && server_->isConnected()) {
        sendToController(j);
        return;
    }
    // No controller: grant automatically.
    sendMessage(svc::session, msg::SessionGrantResources, Json());
}

void SpeakerCommunicator::procReleaseResources(const Json& j) {
    if (server_ && server_->isConnected())
        sendToController(j);
}

void SpeakerCommunicator::procSendPacket(const Json& j) {
    if (server_ && server_->isConnected())
        sendToController(jv::at(j, "packet"));
}

void SpeakerCommunicator::procNotifySessionState(const Json& j) {
    sessionState_ = jv::integer(j, "state");
    if (sessionState_ == kSessionCasting)
        readVolume();  // TCNG: the mixer may have been changed outside the app
    if (server_ && server_->isConnected())
        sendToController(j);
}

// --- Outbound -----------------------------------------------------------------

void SpeakerCommunicator::sendToController(const Json& j) {
    if (!server_ || !server_->isConnected()) {
        log::get()->debug("sendToController: not connected");
        return;
    }
    server_->sendPacket(j.dump());
}

void SpeakerCommunicator::controllerNotifyRequestResult(int requestId, int resultCode) {
    sendToController({{"command", "notifyRequestResult"}, {"requestId", requestId}, {"resultCode", resultCode}});
}

void SpeakerCommunicator::controllerSetVolumeMute() {
    sendToController({{"command", "setVolumeMute"},
                      {"volume", {{"level", volume_.level}, {"mute", volume_.mute}}}});
}

void SpeakerCommunicator::applyVolume() {
    if (config_.volume && !config_.volume->set({volume_.level, volume_.mute}))
        log::get()->warn("volume: {} could not apply level {} mute {}", config_.volume->name(), volume_.level, volume_.mute);
}

void SpeakerCommunicator::readVolume() {
    if (!config_.volume)
        return;
    if (const auto current = config_.volume->current()) {
        volume_.level = current->level;
        volume_.mute = current->mute;
    }
}

void SpeakerCommunicator::controllerNotifyServiceStateChanged(int state) {
    sendToController({{"command", "notifyServiceStateChanged"}, {"serviceState", state}});
}

void SpeakerCommunicator::appNotifyDeviceStatusChanged(int socketId) {
    Json body{{"packet",
               {{"command", "notifyDeviceStatusChanged"},
                {"volume", {{"level", volume_.mute ? 0.0 : volume_.level}, {"mute", volume_.mute}}}}}};
    if (socketId != 0)
        body["socketId"] = socketId;
    sendMessage(svc::clientcomm, msg::SendPacket, body);
}

void SpeakerCommunicator::appNotifyRequestResult(int socketId, int requestId, int resultCode) {
    sendMessage(svc::clientcomm, msg::SendPacket,
                {{"socketId", socketId},
                 {"packet", {{"command", "notifyRequestResult"}, {"requestId", requestId}, {"resultCode", resultCode}}}});
}

void SpeakerCommunicator::mdnsSetModelInfo(const std::string& modelName, const std::string& friendlyName) {
    sendMessage(svc::mdns, msg::MdnsSetConfig,
                {{"packet", {{"modelName", modelName}, {"friendlyName", friendlyName}}}, {"socketId", 0}});
}

void SpeakerCommunicator::mdnsStartServer() {
    sendMessage(svc::mdns, msg::MdnsStartServer, Json());
}

void SpeakerCommunicator::mdnsStopServer() {
    sendMessage(svc::mdns, msg::MdnsStopServer, Json());
}

}  // namespace tcng::spkcon
