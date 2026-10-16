#pragma once
// Shared pieces of the libwebsockets-backed servers.

#include <cstdint>
#include <deque>
#include <functional>
#include <string>

struct lws;

namespace tcng::net {

using ConnectionId = std::uint64_t;

struct ServerCallbacks {
    std::function<void(ConnectionId)> onOpen;
    // WSS: one complete (reassembled) text/binary message. Raw: whatever bytes arrived.
    std::function<void(ConnectionId, std::string)> onData;
    std::function<void(ConnectionId)> onClose;
};

namespace detail {

// Per-connection state, owned by the server, touched only on the loop thread.
struct Conn {
    ConnectionId id = 0;
    lws* wsi = nullptr;
    std::deque<std::string> outbox;
    std::string inbox;       // WSS fragment reassembly
    bool closing = false;
};

}  // namespace detail

}  // namespace tcng::net
