#include "core/ClientCommunicator.hpp"

#include <algorithm>
#include <map>
#include <random>

#include "util/JsonUtil.hpp"
#include "core/MessageIds.hpp"
#include "util/Log.hpp"

namespace tcng::core {
namespace svc = service;

// --- CommandFilter -------------------------------------------------------------

CommandFilter::CommandFilter()
    : nonBlockingCommands_{"startSession", "resumeSession", "endSession", "notifySessionError"} {}

void CommandFilter::addBlockingSocket(std::uint32_t socketId) {
    blockingSockets_.insert(socketId);
}

void CommandFilter::removeBlockingSocket(std::uint32_t socketId) {
    blockingSockets_.erase(socketId);
}

bool CommandFilter::isNonBlocking(const std::string& command) const {
    return std::find(nonBlockingCommands_.begin(), nonBlockingCommands_.end(), command) !=
           nonBlockingCommands_.end();
}

bool CommandFilter::isBlocking(std::uint32_t socketId, const std::string& command) const {
    return blockingSockets_.count(socketId) && !isNonBlocking(command);
}

std::vector<std::uint32_t> CommandFilter::getBlockingSocketList(const std::string& command) const {
    if (isNonBlocking(command))
        return {};
    return {blockingSockets_.begin(), blockingSockets_.end()};
}

// --- MessageTranslator ---------------------------------------------------------

CommandItem getMessageCommandItem(const std::string& command) {
    static const std::map<std::string, CommandItem> table{
        {"play", {svc::playback, msg::PlaybackPlay}},
        {"pause", {svc::playback, msg::PlaybackPause}},
        {"stop", {svc::playback, msg::PlaybackStop}},
        {"seek", {svc::playback, msg::PlaybackSeek}},
        {"startSession", {svc::session, msg::SessionStart}},
        {"resumeSession", {svc::session, msg::SessionResume}},
        {"endSession", {svc::session, msg::SessionEnd}},
        {"next", {svc::queue, msg::QueueNext}},
        {"previous", {svc::queue, msg::QueuePrevious}},
        {"loadMedia", {svc::queue, msg::QueueLoadMedia}},
        {"loadLocalQueue", {svc::queue, msg::QueueLoadLocalQueue}},
        {"loadCloudQueue", {svc::queue, msg::QueueLoadCloudQueue}},
        {"updateServerInfo", {svc::queue, msg::QueueUpdateServerInfo}},
        {"selectQueueItem", {svc::queue, msg::QueueSelectQueueItem}},
        {"setRepeatMode", {svc::queue, msg::QueueSetRepeatMode}},
        {"setShuffle", {svc::queue, msg::QueueSetShuffle}},
        {"refreshQueue", {svc::queue, msg::QueueRefreshQueue}},
        {"respondQueueItems", {svc::queue, msg::QueueRespondQueueItems}},
        {"setVolume", {svc::speaker, msg::SpeakerSetVolume}},
        {"setMute", {svc::speaker, msg::SpeakerSetMute}},
    };
    auto it = table.find(command);
    return it == table.end() ? CommandItem{} : it->second;
}

// --- ClientCommunicator --------------------------------------------------------

ClientCommunicator::ClientCommunicator(net::Loop& loop, Config config)
    : Service(svc::clientcomm), loop_(loop), config_(std::move(config)) {}

ClientCommunicator::~ClientCommunicator() {
    finish();
}

void ClientCommunicator::start() {
    if (!server_) {
        server_ = std::make_unique<net::WebsocketServer>(loop_, net::ServerCallbacks{
            .onOpen = [this](net::ConnectionId c) { onOpen(c); },
            .onData = [this](net::ConnectionId c, std::string p) { onReceivePacket(c, p); },
            .onClose = [this](net::ConnectionId c) { onClose(c); },
        });
        net::WebsocketConfig wss;
        wss.port = config_.port;
        wss.certPath = config_.certificatePath;
        try {
            server_->listen(wss);
            log::get()->info("websocket server listening on port {}", config_.port);
        } catch (const std::exception& e) {
            log::get()->error("websocket server start failed: {}", e.what());
            server_.reset();
        }
    }
    Service::start();
}

void ClientCommunicator::finish() {
    Service::finish();
    if (server_) {
        server_->close();
        server_.reset();
    }
    std::lock_guard lock(mutex_);
    sockets_.clear();
    serverClosing_.clear();
}

std::uint32_t ClientCommunicator::generateId() {
    // Random, but never 0 (0 means "anycast"/"controller") and unique.
    static thread_local std::mt19937 rng{std::random_device{}()};
    for (;;) {
        const std::uint32_t id = rng();
        if (id != 0 && std::none_of(sockets_.begin(), sockets_.end(), [&](const Socket& s) { return s.id == id; }))
            return id;
    }
}

void ClientCommunicator::onOpen(net::ConnectionId conn) {
    std::uint32_t id;
    {
        std::lock_guard lock(mutex_);
        id = generateId();
        sockets_.push_back({conn, id});
        filter_.addBlockingSocket(id);  // every new socket starts blocked
    }
    log::get()->info("{}: open", id);
}

void ClientCommunicator::onReceivePacket(net::ConnectionId conn, const std::string& payload) {
    std::uint32_t socketId = 0;
    bool blocking = false;
    Json j = Json::parse(payload, nullptr, false);
    if (j.is_discarded()) {
        log::get()->error("[ASSERT] onReceivePacket(0)");
        return;
    }
    const std::string command = jv::string(j, "command");
    {
        std::lock_guard lock(mutex_);
        auto it = std::find_if(sockets_.begin(), sockets_.end(), [&](const Socket& s) { return s.conn == conn; });
        if (it == sockets_.end())
            return;
        socketId = it->id;
        blocking = filter_.isBlocking(socketId, command);
    }
    if (blocking) {
        log::get()->info("Block incoming command: {}:{}", socketId, command);
        return;
    }
    // Command name and position only: other packets (startSession, loadCloudQueue) carry tokens.
    if (const auto* position = j.is_object() && j.contains("position") ? &j["position"] : nullptr)
        log::get()->debug("received {}:{} position={}", socketId, command, position->dump());
    else
        log::get()->debug("received {}:{}", socketId, command);
    const CommandItem item = getMessageCommandItem(command);
    if (item.id == 0) {
        log::get()->error("[ASSERT] onReceivePacket: unknown command \"{}\"", command);
        return;
    }
    sendMessage(item.service, item.id, Json{{"socketId", socketId}, {"packet", std::move(j)}});
}

void ClientCommunicator::onClose(net::ConnectionId conn) {
    std::uint32_t socketId = 0;
    bool initiatedBySession = false;
    {
        std::lock_guard lock(mutex_);
        auto it = std::find_if(sockets_.begin(), sockets_.end(), [&](const Socket& s) { return s.conn == conn; });
        if (it == sockets_.end())
            return;
        socketId = it->id;
        sockets_.erase(it);
        filter_.removeBlockingSocket(socketId);
        initiatedBySession = serverClosing_.erase(conn) > 0;
    }
    log::get()->info("{}: close", socketId);
    // A close the session asked for needs no report. Every other close — the phone
    // leaving or the keep-alive timing out — is reported.
    if (!initiatedBySession)
        sendMessage(svc::session, msg::SessionLostConnection, Json{{"socketId", socketId}});
}

void ClientCommunicator::handleMessage(std::uint32_t id, const Json& j) {
    switch (id) {
    case msg::SendPacket: procSendPacket(j); break;
    case msg::ClientcommBlockPacket: procBlockPacket(j); break;
    case msg::ClientcommCloseSocket: procCloseSocket(j); break;
    default: log::get()->warn("clientcomm: unhandled message 0x{:08x}", id); break;
    }
}

void ClientCommunicator::sendTo(const Socket& socket, const std::string& data) {
    if (server_)
        server_->send(socket.conn, data);
}

void ClientCommunicator::procSendPacket(const Json& j) {
    const Json& packet = jv::at(j, "packet");
    const std::string command = jv::string(packet, "command");
    const std::string data = packet.dump();
    const Json& socketIdField = jv::at(j, "socketId");

    std::lock_guard lock(mutex_);
    if (socketIdField.is_null()) {
        auto excluded = filter_.getBlockingSocketList(command);
        if (const Json& except = jv::at(j, "exception_socket"); except.is_number())
            excluded.push_back(static_cast<std::uint32_t>(jv::integer(except)));
        log::get()->debug("multicast: {}", data);
        for (const auto& s : sockets_)
            if (std::find(excluded.begin(), excluded.end(), s.id) == excluded.end())
                sendTo(s, data);
        return;
    }
    const auto socketId = static_cast<std::uint32_t>(socketIdField.is_number() ? socketIdField.get<double>() : 0);
    if (socketId == 0) {
        const auto excluded = filter_.getBlockingSocketList(command);
        for (const auto& s : sockets_) {
            if (std::find(excluded.begin(), excluded.end(), s.id) == excluded.end()) {
                log::get()->debug("anycast: {}", data);
                sendTo(s, data);
                return;
            }
        }
        return;
    }
    auto it = std::find_if(sockets_.begin(), sockets_.end(), [&](const Socket& s) { return s.id == socketId; });
    if (it == sockets_.end() || filter_.isBlocking(socketId, command)) {
        log::get()->debug("unicast dropped ({}): {}", socketId, data);
        return;
    }
    log::get()->debug("unicast: {}", data);
    sendTo(*it, data);
}

void ClientCommunicator::procBlockPacket(const Json& j) {
    const auto socketId = static_cast<std::uint32_t>(jv::number(j, "socketId"));
    std::lock_guard lock(mutex_);
    if (jv::boolean(j, "enable"))
        filter_.addBlockingSocket(socketId);
    else
        filter_.removeBlockingSocket(socketId);
}

void ClientCommunicator::procCloseSocket(const Json& j) {
    const auto socketId = static_cast<std::uint32_t>(jv::number(j, "socketId"));
    std::lock_guard lock(mutex_);
    filter_.removeBlockingSocket(socketId);
    auto it = std::find_if(sockets_.begin(), sockets_.end(), [&](const Socket& s) { return s.id == socketId; });
    if (it == sockets_.end() || !server_)
        return;
    serverClosing_.insert(it->conn);
    server_->disconnect(it->conn);  // queued after any pending sends, e.g. notifySessionEnded
}

}  // namespace tcng::core
