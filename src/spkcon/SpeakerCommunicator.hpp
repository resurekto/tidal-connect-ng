#pragma once
// The "speaker" service: bridges the spkcon controller
// socket and the other services, and owns the volume/mute state.
// TCNG: the app's setVolume/setMute are also applied through an IVolumeControl (--volume);
// the level is read back from it when the service starts and when a session starts casting.

#include <memory>
#include <string>

#include "core/MessageCore.hpp"
#include "media/output/IVolumeControl.hpp"
#include "net/Loop.hpp"
#include "spkcon/SpkconServer.hpp"

namespace tcng::spkcon {

class SpeakerCommunicator : public core::Service, public SpkconServer::Delegate {
public:
    struct Config {
        std::string controllerUnixSocketPath;
        std::shared_ptr<media::IVolumeControl> volume;  // null = not applied
    };

    SpeakerCommunicator(net::Loop& loop, Config config);
    ~SpeakerCommunicator() override;

    // Activator start: keeps the manager for startService/stopService.
    bool start(const std::shared_ptr<core::IServiceManager>& manager) override;
    void start() override;
    void finish() override;

protected:
    void handleMessage(std::uint32_t id, const core::Json& payload) override;

private:
    struct VolumeInfo {
        double level = 0.2;
        bool mute = false;
    };

    // SpkconServer::Delegate (socket thread)
    void onOpen() override;
    void onClose(Reason reason) override;
    void onReceivePacket(const std::string& json) override;

    void procRequestFromController(const core::Json& j);
    void handleCmdSetVolumeMute(const core::Json& j);
    void handleCmdStartService(const core::Json& j);
    void handleCmdStopService();
    void handleCmdCommon(const std::string& cmd, const core::Json& j);

    void procSetVolume(const core::Json& j);
    void procSetMute(const core::Json& j);
    void procRequestResources(const core::Json& j);
    void procReleaseResources(const core::Json& j);
    void procSendPacket(const core::Json& j);
    void procNotifySessionState(const core::Json& j);

    void sendToController(const core::Json& j);
    void controllerNotifyRequestResult(int requestId, int resultCode);
    void controllerSetVolumeMute();
    void applyVolume();      // TCNG: volume_ -> config_.volume
    void readVolume();       // TCNG: config_.volume -> volume_
    void controllerNotifyServiceStateChanged(int state);
    void appNotifyDeviceStatusChanged(int socketId);
    void appNotifyRequestResult(int socketId, int requestId, int resultCode);
    void mdnsSetModelInfo(const std::string& modelName, const std::string& friendlyName);
    void mdnsStartServer();
    void mdnsStopServer();

    net::Loop& loop_;
    Config config_;
    std::weak_ptr<core::IServiceManager> manager_;
    std::unique_ptr<SpkconServer> server_;
    VolumeInfo volume_;     // speaker thread only
    int sessionState_ = 0;  // speaker thread only
};

}  // namespace tcng::spkcon
