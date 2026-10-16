#include "net/WebsocketServer.hpp"

#include <libwebsockets.h>
#include <openssl/ssl.h>

#include <stdexcept>
#include <vector>

#include "util/Log.hpp"

namespace tcng::net {

namespace {

int wssCallback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len) {
    return WebsocketServer::dispatch(wsi, static_cast<int>(reason), user, in, len);
}

// protocols[0] also serves the HTTP upgrade; a client that names no subprotocol
// lands here too.
const lws_protocols kProtocols[] = {
    {"tcng-websocket", wssCallback, sizeof(detail::Conn), 0, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM,
};

}  // namespace

WebsocketServer::WebsocketServer(Loop& loop, ServerCallbacks callbacks)
    : loop_(loop), callbacks_(std::move(callbacks)) {}

WebsocketServer::~WebsocketServer() {
    close();
    delete retry_;
}

void WebsocketServer::listen(const WebsocketConfig& config) {
    config_ = config;

    lws_context_creation_info info{};
    info.port = config_.port;
    info.iface = config_.iface.empty() ? nullptr : config_.iface.c_str();
    info.protocols = kProtocols;
    info.vhost_name = "tcng-websocket";
    info.user = this;
    info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;
    if (config_.tls) {
        info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        info.ssl_cert_filepath = config_.certPath.c_str();
        info.ssl_private_key_filepath =
            config_.keyPath.empty() ? config_.certPath.c_str() : config_.keyPath.c_str();
        if (!config_.cipherList.empty())
            info.ssl_cipher_list = config_.cipherList.c_str();
        info.ssl_options_set = SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1;
    }
    if (config_.pingAfterSecs || config_.hangupAfterSecs) {
        delete retry_;
        retry_ = new lws_retry_bo_t{};
        retry_->secs_since_valid_ping = config_.pingAfterSecs;
        retry_->secs_since_valid_hangup = config_.hangupAfterSecs;
        info.retry_and_idle_policy = retry_;
    }

    vhost_ = loop_.createVhost(info);
    if (!vhost_)
        throw std::runtime_error("WSS server: cannot listen on port " + std::to_string(config_.port));
}

void WebsocketServer::close() {
    if (!vhost_)
        return;
    loop_.call([this] {
        lws_vhost_destroy(vhost_);
        vhost_ = nullptr;
        conns_.clear();
    });
}

void WebsocketServer::send(ConnectionId id, std::string text) {
    loop_.post([this, id, text = std::move(text)]() mutable {
        auto it = conns_.find(id);
        if (it == conns_.end())
            return;
        it->second->outbox.push_back(std::move(text));
        lws_callback_on_writable(it->second->wsi);
    });
}

void WebsocketServer::disconnect(ConnectionId id) {
    loop_.post([this, id] {
        auto it = conns_.find(id);
        if (it == conns_.end())
            return;
        it->second->closing = true;
        lws_callback_on_writable(it->second->wsi);
    });
}

int WebsocketServer::dispatch(lws* wsi, int reason, void* user, void* in, std::size_t len) {
    auto* self = static_cast<WebsocketServer*>(lws_get_vhost_user(lws_get_vhost(wsi)));
    if (!self)
        return lws_callback_http_dummy(wsi, static_cast<lws_callback_reasons>(reason), user, in, len);
    return self->handle(wsi, reason, user, in, len);
}

int WebsocketServer::handle(lws* wsi, int reason, void* user, void* in, std::size_t len) {
    auto* conn = static_cast<detail::Conn*>(user);
    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED: {
        new (conn) detail::Conn();
        conn->id = nextId_++;
        conn->wsi = wsi;
        conns_[conn->id] = conn;
        if (callbacks_.onOpen)
            callbacks_.onOpen(conn->id);
        return 0;
    }
    case LWS_CALLBACK_RECEIVE: {
        conn->inbox.append(static_cast<const char*>(in), len);
        if (!lws_is_final_fragment(wsi) || lws_remaining_packet_payload(wsi) > 0)
            return 0;
        std::string message;
        message.swap(conn->inbox);
        if (callbacks_.onData)
            callbacks_.onData(conn->id, std::move(message));
        return 0;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE: {
        if (conn->outbox.empty()) {
            if (!conn->closing)
                return 0;
            static const unsigned char kReason[] = " ";
            lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, const_cast<unsigned char*>(kReason), 1);
            return -1;
        }
        const std::string& text = conn->outbox.front();
        std::vector<unsigned char> buf(LWS_PRE + text.size());
        std::copy(text.begin(), text.end(), buf.begin() + LWS_PRE);
        const int n = lws_write(wsi, buf.data() + LWS_PRE, text.size(), LWS_WRITE_TEXT);
        conn->outbox.pop_front();
        if (n < static_cast<int>(text.size()))
            return -1;
        if (!conn->outbox.empty() || conn->closing)
            lws_callback_on_writable(wsi);
        return 0;
    }
    case LWS_CALLBACK_CLOSED: {
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
