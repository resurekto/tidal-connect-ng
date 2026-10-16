#include "core/SessionManager.hpp"

#include <algorithm>
#include <random>

#include "util/JsonUtil.hpp"
#include "core/MessageIds.hpp"
#include "util/Log.hpp"

namespace tcng::core {

namespace {

constexpr int kResultSuccess = 0;
constexpr int kErrorIllegalState = 1002;
constexpr int kErrorNoSession = 1101;
constexpr int kErrorInvalidSessionId = 1102;

const char* stateName(SessionState s) {
    switch (s) {
    case SessionState::Idle: return "IDLE";
    case SessionState::Starting: return "STARTING";
    case SessionState::Casting: return "CASTING";
    case SessionState::Switching: return "SWITCHING";
    case SessionState::Canceling: return "CANCELING";
    }
    return "?";
}

std::uint32_t socketIdOf(const Json& j) {
    return static_cast<std::uint32_t>(jv::number(j, "socketId"));
}

}  // namespace

// --- Session -------------------------------------------------------------------

Session::Session(std::string appId, std::string appName, std::string credential)
    : id_(generateId()), appId_(std::move(appId)), appName_(std::move(appName)), credential_(std::move(credential)) {}

std::string Session::generateId() {
    std::random_device rd;
    return std::to_string(static_cast<std::uint32_t>(rd()));
}

void Session::bindSocket(std::uint32_t socketId) {
    if (std::find(sockets_.begin(), sockets_.end(), socketId) == sockets_.end())
        sockets_.push_back(socketId);
}

void Session::unbindSocket(std::uint32_t socketId) {
    auto it = std::find(sockets_.begin(), sockets_.end(), socketId);
    if (it != sockets_.end())
        sockets_.erase(it);
}

// --- SessionManager --------------------------------------------------------------

SessionManager::SessionManager() : Service(service::session) {}

SessionManager::~SessionManager() {
    finish();
}

void SessionManager::finish() {
    Service::finish();
    state_ = SessionState::Idle;  // not broadcast
}

void SessionManager::setState(SessionState state, const std::string& appId, const std::string& appName) {
    log::get()->info("state: {} -> {}", stateName(state_), stateName(state));
    state_ = state;
    Json body{{"command", "notifySessionState"}, {"state", static_cast<int>(state)}};
    if (!appId.empty())
        body["appInfo"] = {{"appId", appId}, {"appName", appName}};
    sendMessage(std::vector<std::string>{service::playback, service::speaker}, msg::NotifySessionState, body);
}

void SessionManager::handleMessage(std::uint32_t id, const Json& j) {
    switch (id) {
    case msg::SessionStart: procStartSession(j); break;
    case msg::SessionEnd: procEndSession(j); break;
    case msg::SessionResume: procResumeSession(j); break;
    case msg::SessionGrantResources: procGrantResources(j); break;
    case msg::SessionRevokeResources: procRevokeResources(j); break;
    case msg::SessionLostConnection: procLostConnection(j); break;
    default: log::get()->warn("session: unhandled message 0x{:08x}", id); break;
    }
}

// --- Handlers ---------------------------------------------------------------------

void SessionManager::procStartSession(const Json& j) {
    const auto sock = socketIdOf(j);
    const Json& packet = jv::at(j, "packet");
    const auto appId = jv::string(packet, "appId");
    const auto appName = jv::string(packet, "appName");
    const auto cred = jv::string(packet, "sessionCredential");
    log::get()->info("{}: {}, aid={}, sc={}", "startSession", sock, appId, cred);

    switch (state_) {
    case SessionState::Idle:
        sessionStart(appId, appName, cred, sock);
        break;
    case SessionState::Casting:
        if (session_ && cred == session_->getCredential())
            sessionJoin(sock);
        else
            sessionSwitch(appId, appName, cred, sock);
        break;
    default:
        log::get()->warn("Illegal State: {}", stateName(state_));
        notifyErrorAndClose(sock, "startSession", kErrorIllegalState, true);
        break;
    }
}

void SessionManager::procEndSession(const Json& j) {
    const auto sock = socketIdOf(j);
    const Json& packet = jv::at(j, "packet");
    const auto sessionId = jv::string(packet, "sessionId");
    const bool stopCasting = jv::boolean(packet, "stopCasting");
    log::get()->info("{}: {}, sid={}, stop={}", "endSession", sock, sessionId, stopCasting);

    switch (state_) {
    case SessionState::Idle:
        log::get()->warn("No Session");
        notifyErrorAndClose(sock, "endSession", kErrorNoSession, true);
        break;
    case SessionState::Starting:
        sessionCancelStart(sock);
        break;
    case SessionState::Casting:
        if (!session_ || sessionId != session_->getId()) {
            log::get()->warn("Invalid SessionId: {} != {}(current)", sessionId, session_ ? session_->getId() : "");
            notifyErrorAndClose(sock, "endSession", kErrorInvalidSessionId, true);
        } else if (stopCasting) {
            sessionEnd(sock, 0);
        } else {
            sessionLeave(sock);
        }
        break;
    case SessionState::Canceling:
        log::get()->warn("Illegal State: {}", stateName(state_));
        notifyErrorAndClose(sock, "endSession", kErrorIllegalState, true);
        break;
    case SessionState::Switching:
        log::get()->critical("[ASSERT] procEndSession: state SWITCHING");
        break;
    }
}

void SessionManager::procResumeSession(const Json& j) {
    const auto sock = socketIdOf(j);
    const auto sessionId = jv::string(jv::at(j, "packet"), "sessionId");
    log::get()->info("{}: {}, sid={}", "resumeSession", sock, sessionId);

    switch (state_) {
    case SessionState::Idle:
        log::get()->warn("No Session: {}", sessionId);
        notifyErrorAndClose(sock, "resumeSession", kErrorNoSession, true);
        break;
    case SessionState::Casting:
        if (session_ && sessionId == session_->getId()) {
            session_->bindSocket(sock);
            notifyResumed(sock, sessionId);
            multiRequestStatusNotification({service::queue, service::speaker}, sock);
        } else {
            log::get()->warn("Invalid SessionId: {} != {}(current)", sessionId, session_ ? session_->getId() : "");
            notifyErrorAndClose(sock, "resumeSession", kErrorInvalidSessionId, true);
        }
        break;
    default:
        log::get()->warn("Illegal State: {}", stateName(state_));
        notifyErrorAndClose(sock, "resumeSession", kErrorIllegalState, true);
        break;
    }
}

void SessionManager::procGrantResources(const Json& j) {
    const int requestId = jv::integer(jv::at(j, "packet"), "requestId");
    speakerNotifyRequestResult(requestId, kResultSuccess);
    switch (state_) {
    case SessionState::Starting: sessionStartCasting(); break;
    case SessionState::Canceling: sessionCancelEnd(); break;
    case SessionState::Switching: log::get()->critical("[ASSERT] procGrantResources: state SWITCHING"); break;
    default: log::get()->warn("Illegal State: {}", stateName(state_)); break;
    }
}

void SessionManager::procRevokeResources(const Json& j) {
    const int requestId = jv::integer(jv::at(j, "packet"), "requestId");
    log::get()->info("{}, {}", "revokeResources", requestId);
    switch (state_) {
    case SessionState::Idle:
    case SessionState::Canceling:
        log::get()->warn("Illegal State: {}", stateName(state_));
        speakerNotifyRequestResult(requestId, kErrorIllegalState);
        break;
    case SessionState::Casting:
        sessionEnd(0, requestId);
        break;
    default:
        log::get()->critical("[ASSERT] procRevokeResources: Illegal State: {}", stateName(state_));
        break;
    }
}

void SessionManager::procLostConnection(const Json& j) {
    const auto sock = socketIdOf(j);
    log::get()->info("{}: {}", "lostConnection", sock);
    switch (state_) {
    case SessionState::Casting:
        if (session_)
            session_->unbindSocket(sock);
        break;
    case SessionState::Canceling:
        setState(SessionState::Canceling, "", "");
        session_.reset();
        break;
    default:
        break;
    }
}

// --- Transitions -------------------------------------------------------------------

void SessionManager::sessionStart(const std::string& appId, const std::string& appName, const std::string& cred,
                                  std::uint32_t sock) {
    setState(SessionState::Starting, appId, appName);
    session_ = std::make_unique<Session>(appId, appName, cred);
    session_->bindSocket(sock);
    speakerRequestResources();
}

void SessionManager::sessionJoin(std::uint32_t sock) {
    session_->bindSocket(sock);
    notifyStarted(sock, true);
    multiRequestStatusNotification({service::queue, service::speaker}, sock);
}

void SessionManager::sessionSwitch(const std::string& appId, const std::string& appName, const std::string& cred,
                                   std::uint32_t sock) {
    setState(SessionState::Switching, appId, appName);
    queuePurge();
    if (session_) {
        const auto old = session_->getSocketList();
        for (auto s : old)
            notifyEndedAndClose(s, false);
    }
    session_ = std::make_unique<Session>(appId, appName, cred);
    session_->bindSocket(sock);
    notifyStarted(sock, false);
    setState(SessionState::Casting, appId, appName);
}

void SessionManager::sessionLeave(std::uint32_t sock) {
    session_->unbindSocket(sock);
    notifyEndedAndClose(sock, true);
}

void SessionManager::sessionEnd(std::uint32_t sock, int requestId) {
    queuePurge();
    if (session_) {
        const auto sockets = session_->getSocketList();
        for (auto s : sockets)
            notifyEndedAndClose(s, false);
    }
    session_.reset();
    speakerReleaseResources();
    setState(SessionState::Idle, "", "");
    if (sock == 0)
        speakerNotifyRequestResult(requestId, kResultSuccess);
}

void SessionManager::sessionCancelStart(std::uint32_t sock) {
    setState(SessionState::Canceling, "", "");
    clientcommBlockPacket(sock, false);
    notifyEndedAndClose(sock, false);
    session_.reset();
}

void SessionManager::sessionCancelEnd() {
    speakerReleaseResources();
    setState(SessionState::Idle, "", "");
}

void SessionManager::sessionStartCasting() {
    if (!session_ || session_->getSocketList().empty()) {
        log::get()->critical("[ASSERT] sessionStartCasting: no socket");
        return;
    }
    const auto first = session_->getSocketList().front();
    notifyStarted(first, false);
    setState(SessionState::Casting, session_->getAppId(), session_->getAppName());
    multiRequestStatusNotification({service::speaker}, first);
}

// --- Outbound ----------------------------------------------------------------------

void SessionManager::clientcommBlockPacket(std::uint32_t sock, bool enable) {
    sendMessage(service::clientcomm, msg::ClientcommBlockPacket, {{"socketId", sock}, {"enable", enable}});
}

void SessionManager::speakerNotifyRequestResult(int requestId, int resultCode) {
    sendMessage(service::speaker, msg::SendPacket,
                {{"packet", {{"command", "notifyRequestResult"}, {"requestId", requestId}, {"resultCode", resultCode}}}});
}

void SessionManager::speakerRequestResources() {
    sendMessage(service::speaker, msg::SpeakerRequestResources, {{"command", "requestResources"}});
}

void SessionManager::speakerReleaseResources() {
    sendMessage(service::speaker, msg::SpeakerReleaseResources, {{"command", "releaseResources"}});
}

void SessionManager::queuePurge() {
    sendMessage(service::queue, msg::QueuePurge, Json());
}

void SessionManager::multiRequestStatusNotification(const std::vector<std::string>& targets, std::uint32_t sock) {
    sendMessage(targets, msg::RequestStatusNotification, {{"socketId", sock}});
}

void SessionManager::notifyStarted(std::uint32_t sock, bool joined) {
    clientcommBlockPacket(sock, false);
    sendMessage(service::clientcomm, msg::SendPacket,
                {{"socketId", sock},
                 {"packet", {{"command", "notifySessionStarted"}, {"sessionId", session_->getId()}, {"joined", joined}}}});
}

void SessionManager::notifyResumed(std::uint32_t sock, const std::string& sessionId) {
    clientcommBlockPacket(sock, false);
    sendMessage(service::clientcomm, msg::SendPacket,
                {{"socketId", sock}, {"packet", {{"command", "notifySessionResumed"}, {"sessionId", sessionId}}}});
}

void SessionManager::notifyEndedAndClose(std::uint32_t sock, bool suspended) {
    const std::string sessionId = session_ ? session_->getId() : std::string();
    sendMessage(service::clientcomm, msg::SendPacket,
                {{"socketId", sock},
                 {"packet", {{"command", "notifySessionEnded"}, {"sessionId", sessionId}, {"suspended", suspended}}}});
    sendMessage(service::clientcomm, msg::ClientcommCloseSocket, {{"socketId", sock}});
}

void SessionManager::notifyErrorAndClose(std::uint32_t sock, const std::string& requestedCommand, int errorCode,
                                         bool close) {
    sendMessage(service::clientcomm, msg::SendPacket,
                {{"packet", {{"command", "notifySessionError"},
                             {"requestedCommand", requestedCommand},
                             {"errorCode", errorCode}}}});
    if (close)
        sendMessage(service::clientcomm, msg::ClientcommCloseSocket, {{"socketId", sock}});
}

}  // namespace tcng::core
