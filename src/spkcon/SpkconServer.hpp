#pragma once
// The controller-facing Unix socket server. One active
// controller at a time; a new connection replaces the previous one. Frames are
// FF 02 | len | JSON | FF 03 (SpkconPacket).

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "net/Loop.hpp"
#include "net/UnixServer.hpp"
#include "spkcon/SpkconPacket.hpp"

namespace tcng::spkcon {

enum class ConnectionState : int { Disconnected = 0, Connecting = 1, Connected = 2, Disconnecting = 3 };

enum class Reason : int { NoError = 0, ConnectionError = 1, Disconnected = 2, ReceiveError = 3, Stopped = 4 };

class SpkconServer {
public:
    class Delegate {
    public:
        virtual ~Delegate() = default;
        virtual void onOpen() = 0;
        virtual void onClose(Reason reason) = 0;
        virtual void onReceivePacket(const std::string& json) = 0;
    };

    SpkconServer(net::Loop& loop, Delegate& delegate);
    ~SpkconServer();

    // Listens on path (a stale socket file is removed first). Returns false on failure.
    bool start(const std::string& path);
    void stop();

    // Queues one JSON payload for the controller; dropped if none is connected.
    bool sendPacket(const std::string& json);

    ConnectionState getConnectionState() const { return state_.load(); }
    bool isConnected() const { return getConnectionState() == ConnectionState::Connected; }

private:
    void updateConnectionState(ConnectionState state);
    void onOpen(net::ConnectionId id);
    void onData(net::ConnectionId id, std::string bytes);
    void onClose(net::ConnectionId id);

    net::Loop& loop_;
    Delegate& delegate_;
    std::unique_ptr<net::UnixServer> server_;
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};

    std::mutex mutex_;
    net::ConnectionId client_ = 0;  // the active controller
    std::map<net::ConnectionId, std::unique_ptr<FrameDecoder>> decoders_;  // loop thread only
};

}  // namespace tcng::spkcon
