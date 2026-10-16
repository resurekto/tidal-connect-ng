#pragma once
// WebSocket-over-TLS server on the Loop (the phone/app channel). Messages are passed
// up whole: fragments are reassembled before onData.

#include <cstdint>
#include <map>
#include <string>

#include "net/Connection.hpp"
#include "net/Loop.hpp"

struct lws_vhost;
struct lws;
struct lws_retry_bo;

namespace tcng::net {

struct WebsocketConfig {
    std::uint16_t port = 0;
    std::string iface;              // empty: all interfaces
    std::string certPath;           // PEM: certificate chain (and key, if keyPath is empty)
    std::string keyPath;            // PEM private key; empty: read from certPath
    std::string cipherList;         // empty: library default
    bool tls = true;                // TLS 1.2+ only
    // Keep-alive: ping an idle peer after pingAfterSecs, close after hangupAfterSecs
    // without traffic. 0 disables.
    std::uint16_t pingAfterSecs = 30;
    std::uint16_t hangupAfterSecs = 40;
};

class WebsocketServer {
public:
    WebsocketServer(Loop& loop, ServerCallbacks callbacks);
    ~WebsocketServer();

    // Opens the listening socket. Throws on failure.
    void listen(const WebsocketConfig& config);
    void close();

    // Thread-safe.
    void send(ConnectionId id, std::string text);
    // Closes with status 1000 and reason " ".
    void disconnect(ConnectionId id);

    static int dispatch(struct lws* wsi, int reason, void* user, void* in, std::size_t len);

private:
    int handle(struct lws* wsi, int reason, void* user, void* in, std::size_t len);

    Loop& loop_;
    ServerCallbacks callbacks_;
    lws_vhost* vhost_ = nullptr;
    WebsocketConfig config_;
    struct lws_retry_bo* retry_ = nullptr;
    std::map<ConnectionId, detail::Conn*> conns_;  // loop thread only
    ConnectionId nextId_ = 1;
};

}  // namespace tcng::net
