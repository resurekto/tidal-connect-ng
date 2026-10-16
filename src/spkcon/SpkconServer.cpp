#include "spkcon/SpkconServer.hpp"

#include "util/Log.hpp"

namespace tcng::spkcon {

SpkconServer::SpkconServer(net::Loop& loop, Delegate& delegate) : loop_(loop), delegate_(delegate) {}

SpkconServer::~SpkconServer() {
    stop();
}

void SpkconServer::updateConnectionState(ConnectionState state) {
    const auto old = state_.exchange(state);
    if (old == state)
        log::get()->debug("same state:{}", static_cast<int>(state));
    else
        log::get()->debug("updateConnectionState:{}->{}", static_cast<int>(old), static_cast<int>(state));
}

bool SpkconServer::start(const std::string& path) {
    log::get()->debug("SpkconServer start");
    if (getConnectionState() != ConnectionState::Disconnected) {
        log::get()->warn("already connected state:{}", static_cast<int>(getConnectionState()));
        return false;
    }
    updateConnectionState(ConnectionState::Connecting);
    server_ = std::make_unique<net::UnixServer>(loop_, net::ServerCallbacks{
        .onOpen = [this](net::ConnectionId id) { onOpen(id); },
        .onData = [this](net::ConnectionId id, std::string bytes) { onData(id, std::move(bytes)); },
        .onClose = [this](net::ConnectionId id) { onClose(id); },
    });
    try {
        server_->listen(path);
    } catch (const std::exception& e) {
        log::get()->error("socket bind error. path={} error={}", path, e.what());
        server_.reset();
        updateConnectionState(ConnectionState::Disconnected);
        return false;
    }
    log::get()->info("wait connect.");
    return true;
}

void SpkconServer::stop() {
    if (!server_)
        return;
    updateConnectionState(ConnectionState::Disconnecting);
    server_->close();  // closes connections and removes the socket file
    server_.reset();
    {
        std::lock_guard lock(mutex_);
        client_ = 0;
    }
    updateConnectionState(ConnectionState::Disconnected);
    delegate_.onClose(Reason::Stopped);
}

bool SpkconServer::sendPacket(const std::string& json) {
    net::ConnectionId client;
    {
        std::lock_guard lock(mutex_);
        client = client_;
    }
    if (client == 0 || !isConnected()) {
        log::get()->debug("not connected.");
        return false;
    }
    if (json.empty()) {
        log::get()->warn("no data.");
        return false;
    }
    auto frame = encodeFrame(json);
    if (!frame) {
        log::get()->error("packet too large for spkcon framing: {} bytes", json.size());
        return false;
    }
    server_->send(client, std::move(*frame));
    return true;
}

void SpkconServer::onOpen(net::ConnectionId id) {
    net::ConnectionId previous;
    {
        std::lock_guard lock(mutex_);
        previous = client_;
        client_ = id;
    }
    // Last connection wins; the old one is closed.
    if (previous != 0) {
        log::get()->warn("controller replaced: closing previous connection");
        server_->disconnect(previous);
    }
    decoders_[id] = std::make_unique<FrameDecoder>(
        [this, id](std::string payload) {
            bool active;
            {
                std::lock_guard lock(mutex_);
                active = (client_ == id);
            }
            if (active)
                delegate_.onReceivePacket(payload);
        },
        [](const std::string& what) { log::get()->warn("{}", what); });
    log::get()->info("connected");
    updateConnectionState(ConnectionState::Connected);
    delegate_.onOpen();
}

void SpkconServer::onData(net::ConnectionId id, std::string bytes) {
    auto it = decoders_.find(id);
    if (it != decoders_.end())
        it->second->feed(bytes);
}

void SpkconServer::onClose(net::ConnectionId id) {
    decoders_.erase(id);
    bool wasActive;
    {
        std::lock_guard lock(mutex_);
        wasActive = (client_ == id);
        if (wasActive)
            client_ = 0;
    }
    if (!wasActive)
        return;
    if (getConnectionState() == ConnectionState::Connected)
        updateConnectionState(ConnectionState::Connecting);  // listening again
    delegate_.onClose(Reason::Disconnected);
}

}  // namespace tcng::spkcon
