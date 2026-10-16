#pragma once
// The "session" service: who is casting, which sockets belong to the session, and the
// resource handshake with the speaker controller.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/MessageCore.hpp"

namespace tcng::core {

enum class SessionState : int { Idle = 0, Starting = 1, Casting = 2, Switching = 3, Canceling = 4 };

class Session {
public:
    Session(std::string appId, std::string appName, std::string credential);

    const std::string& getId() const { return id_; }
    const std::string& getAppId() const { return appId_; }
    const std::string& getAppName() const { return appName_; }
    const std::string& getCredential() const { return credential_; }
    const std::vector<std::uint32_t>& getSocketList() const { return sockets_; }

    void bindSocket(std::uint32_t socketId);
    void unbindSocket(std::uint32_t socketId);

private:
    static std::string generateId();

    std::string id_;
    std::string appId_;
    std::string appName_;
    std::string credential_;
    std::vector<std::uint32_t> sockets_;
};

class SessionManager : public Service {
public:
    SessionManager();
    ~SessionManager() override;

    void finish() override;
    SessionState getState() const { return state_; }

protected:
    void handleMessage(std::uint32_t id, const Json& payload) override;

private:
    void setState(SessionState state, const std::string& appId, const std::string& appName);

    void procStartSession(const Json& j);
    void procEndSession(const Json& j);
    void procResumeSession(const Json& j);
    void procGrantResources(const Json& j);
    void procRevokeResources(const Json& j);
    void procLostConnection(const Json& j);

    void sessionStart(const std::string& appId, const std::string& appName, const std::string& cred,
                      std::uint32_t sock);
    void sessionJoin(std::uint32_t sock);
    void sessionSwitch(const std::string& appId, const std::string& appName, const std::string& cred,
                       std::uint32_t sock);
    void sessionLeave(std::uint32_t sock);
    void sessionEnd(std::uint32_t sock, int requestId);
    void sessionCancelStart(std::uint32_t sock);
    void sessionCancelEnd();
    void sessionStartCasting();

    void clientcommBlockPacket(std::uint32_t sock, bool enable);
    void speakerNotifyRequestResult(int requestId, int resultCode);
    void speakerRequestResources();
    void speakerReleaseResources();
    void queuePurge();
    void multiRequestStatusNotification(const std::vector<std::string>& targets, std::uint32_t sock);
    void notifyStarted(std::uint32_t sock, bool joined);
    void notifyResumed(std::uint32_t sock, const std::string& sessionId);
    void notifyEndedAndClose(std::uint32_t sock, bool suspended);
    void notifyErrorAndClose(std::uint32_t sock, const std::string& requestedCommand, int errorCode, bool close);

    SessionState state_ = SessionState::Idle;  // session thread only
    std::unique_ptr<Session> session_;
};

}  // namespace tcng::core
