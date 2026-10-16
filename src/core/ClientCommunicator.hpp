#pragma once
// The "clientcomm" service: the phone-facing WSS server. Inbound JSON is routed to services by its
// "command"; outbound envelopes are unicast, multicast or anycast to sockets.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/MessageCore.hpp"
#include "net/Loop.hpp"
#include "net/WebsocketServer.hpp"

namespace tcng::core {

// Per-socket command blocking: a socket is blocked until it owns a session; blocked
// sockets may only send the non-blocking commands (and only receive those).
class CommandFilter {
public:
    CommandFilter();
    void addBlockingSocket(std::uint32_t socketId);
    void removeBlockingSocket(std::uint32_t socketId);
    bool isBlocking(std::uint32_t socketId, const std::string& command) const;
    // Sockets a packet with this command must not reach.
    std::vector<std::uint32_t> getBlockingSocketList(const std::string& command) const;

private:
    bool isNonBlocking(const std::string& command) const;

    std::vector<std::string> nonBlockingCommands_;
    std::set<std::uint32_t> blockingSockets_;
};

struct CommandItem {
    std::string service;
    std::uint32_t id = 0;
};

// MessageTranslator: app command → (service, message id). id 0 = unknown.
CommandItem getMessageCommandItem(const std::string& command);

class ClientCommunicator : public core::Service {
public:
    struct Config {
        std::uint16_t port = 2019;
        std::string certificatePath;  // PEM with certificate chain and private key
    };

    ClientCommunicator(net::Loop& loop, Config config);
    ~ClientCommunicator() override;

    void start() override;
    void finish() override;

protected:
    void handleMessage(std::uint32_t id, const core::Json& payload) override;

private:
    struct Socket {
        net::ConnectionId conn = 0;
        std::uint32_t id = 0;
    };

    // WebsocketServer callbacks (loop thread)
    void onOpen(net::ConnectionId conn);
    void onReceivePacket(net::ConnectionId conn, const std::string& payload);
    void onClose(net::ConnectionId conn);

    void procSendPacket(const core::Json& j);
    void procBlockPacket(const core::Json& j);
    void procCloseSocket(const core::Json& j);

    std::uint32_t generateId();
    void sendTo(const Socket& socket, const std::string& data);

    net::Loop& loop_;
    Config config_;
    std::unique_ptr<net::WebsocketServer> server_;

    mutable std::mutex mutex_;     // sockets_, filter_, serverClosing_
    std::vector<Socket> sockets_;  // connection order (anycast picks the first eligible)
    CommandFilter filter_;
    std::set<net::ConnectionId> serverClosing_;  // closes we initiated on session request
};

}  // namespace tcng::core
