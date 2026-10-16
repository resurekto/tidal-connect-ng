#include "net/UnixServer.hpp"

#include <libwebsockets.h>

#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "util/Log.hpp"

namespace tcng::net {

namespace {

int rawCallback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len) {
    return UnixServer::dispatch(wsi, static_cast<int>(reason), user, in, len);
}

const lws_protocols kProtocols[] = {
    {"tcng-raw", rawCallback, sizeof(detail::Conn), 0, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM,
};

}  // namespace

UnixServer::UnixServer(Loop& loop, ServerCallbacks callbacks)
    : loop_(loop), callbacks_(std::move(callbacks)) {}

UnixServer::~UnixServer() {
    close();
}

void UnixServer::listen(const std::string& path) {
    // sun_path is 104 bytes on macOS, 108 on Linux and Windows; lws only logs the error.
    constexpr std::size_t kMaxPath = 103;
    if (path.empty() || path.size() > kMaxPath)
        throw std::runtime_error("Unix socket path must be 1.." + std::to_string(kMaxPath) + " bytes: " + path);
    path_ = path;
    std::error_code ec;
    std::filesystem::remove(path_, ec);

    lws_context_creation_info info{};
    info.port = 0;  // ignored for Unix sockets, but must not be CONTEXT_PORT_NO_LISTEN
    info.iface = path_.c_str();
    info.protocols = kProtocols;
    info.vhost_name = "tcng-unix";
    info.user = this;
    info.options = LWS_SERVER_OPTION_UNIX_SOCK | LWS_SERVER_OPTION_ONLY_RAW;

    vhost_ = loop_.createVhost(info);
    if (!vhost_)
        throw std::runtime_error("Unix socket server: cannot listen on " + path_);
}

void UnixServer::close() {
    if (!vhost_)
        return;
    loop_.call([this] {
        lws_vhost_destroy(vhost_);
        vhost_ = nullptr;
        conns_.clear();
    });
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

void UnixServer::send(ConnectionId id, std::string bytes) {
    loop_.post([this, id, bytes = std::move(bytes)]() mutable {
        auto it = conns_.find(id);
        if (it == conns_.end())
            return;
        it->second->outbox.push_back(std::move(bytes));
        lws_callback_on_writable(it->second->wsi);
    });
}

void UnixServer::disconnect(ConnectionId id) {
    loop_.post([this, id] {
        auto it = conns_.find(id);
        if (it == conns_.end())
            return;
        it->second->closing = true;
        lws_callback_on_writable(it->second->wsi);
    });
}

int UnixServer::dispatch(lws* wsi, int reason, void* user, void* in, std::size_t len) {
    auto* self = static_cast<UnixServer*>(lws_get_vhost_user(lws_get_vhost(wsi)));
    if (!self)
        return lws_callback_http_dummy(wsi, static_cast<lws_callback_reasons>(reason), user, in, len);
    return self->handle(wsi, reason, user, in, len);
}

int UnixServer::handle(lws* wsi, int reason, void* user, void* in, std::size_t len) {
    auto* conn = static_cast<detail::Conn*>(user);
    switch (reason) {
    case LWS_CALLBACK_RAW_ADOPT: {
        new (conn) detail::Conn();
        conn->id = nextId_++;
        conn->wsi = wsi;
        conns_[conn->id] = conn;
        if (callbacks_.onOpen)
            callbacks_.onOpen(conn->id);
        return 0;
    }
    case LWS_CALLBACK_RAW_RX:
        if (callbacks_.onData)
            callbacks_.onData(conn->id, std::string(static_cast<const char*>(in), len));
        return 0;
    case LWS_CALLBACK_RAW_WRITEABLE: {
        if (conn->outbox.empty())
            return conn->closing ? -1 : 0;
        const std::string& bytes = conn->outbox.front();
        std::vector<unsigned char> buf(LWS_PRE + bytes.size());
        std::copy(bytes.begin(), bytes.end(), buf.begin() + LWS_PRE);
        const int n = lws_write(wsi, buf.data() + LWS_PRE, bytes.size(), LWS_WRITE_RAW);
        conn->outbox.pop_front();
        if (n < static_cast<int>(bytes.size()))
            return -1;
        if (!conn->outbox.empty() || conn->closing)
            lws_callback_on_writable(wsi);
        return 0;
    }
    case LWS_CALLBACK_RAW_CLOSE: {
        const ConnectionId id = conn->id;
        if (id == 0)
            return 0;
        conns_.erase(id);
        conn->~Conn();
        if (callbacks_.onClose)
            callbacks_.onClose(id);
        return 0;
    }
    default:
        return lws_callback_http_dummy(wsi, static_cast<lws_callback_reasons>(reason), user, in, len);
    }
}

}  // namespace tcng::net
