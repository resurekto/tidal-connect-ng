#pragma once
// Raw (non-WebSocket) stream server on a Unix domain socket, on the Loop. Used for the
// spkcon controller channel. Bytes are passed up as they arrive; framing is the
// caller's job. AF_UNIX works on Linux, macOS and Windows 10+ (afunix.h).

#include <map>
#include <string>

#include "net/Connection.hpp"
#include "net/Loop.hpp"

struct lws_vhost;
struct lws;

namespace tcng::net {

class UnixServer {
public:
    UnixServer(Loop& loop, ServerCallbacks callbacks);
    ~UnixServer();

    // Removes a stale socket file at path, then listens. Throws on failure.
    void listen(const std::string& path);
    void close();

    // Thread-safe.
    void send(ConnectionId id, std::string bytes);
    void disconnect(ConnectionId id);

    static int dispatch(struct lws* wsi, int reason, void* user, void* in, std::size_t len);

private:
    int handle(struct lws* wsi, int reason, void* user, void* in, std::size_t len);

    Loop& loop_;
    ServerCallbacks callbacks_;
    lws_vhost* vhost_ = nullptr;
    std::string path_;
    std::map<ConnectionId, detail::Conn*> conns_;  // loop thread only
    ConnectionId nextId_ = 1;
};

}  // namespace tcng::net
