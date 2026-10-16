#pragma once
// Minimal wss:// client on its own libwebsockets context and thread (tests only).

#include <libwebsockets.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tcng::test {

class WsClient {
public:
    explicit WsClient(int port) {
        lws_context_creation_info info{};
        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols();
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        info.user = this;
        context_ = lws_create_context(&info);

        lws_client_connect_info ci{};
        ci.context = context_;
        ci.address = "127.0.0.1";
        ci.port = port;
        ci.path = "/";
        ci.host = "localhost";
        ci.origin = "localhost";
        ci.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK |
                            LCCSCF_ALLOW_INSECURE;
        ci.protocol = nullptr;
        ci.userdata = this;
        ci.pwsi = &wsi_;
        lws_client_connect_via_info(&ci);
        thread_ = std::thread([this] {
            while (!stop_)
                lws_service(context_, 0);
        });
    }

    ~WsClient() {
        stop_ = true;
        lws_cancel_service(context_);
        thread_.join();
        lws_context_destroy(context_);
    }

    bool waitConnected(std::chrono::milliseconds t = std::chrono::seconds(3)) {
        std::unique_lock l(m_);
        return cv_.wait_for(l, t, [&] { return connected_ || closed_; }) && connected_;
    }

    bool waitClosed(std::chrono::milliseconds t = std::chrono::seconds(3)) {
        std::unique_lock l(m_);
        return cv_.wait_for(l, t, [&] { return closed_; });
    }

    void send(std::string text) {
        {
            std::lock_guard l(m_);
            out_.push_back(std::move(text));
        }
        lws_cancel_service(context_);
    }

    // Next received text message, or "" on timeout.
    std::string receive(std::chrono::milliseconds t = std::chrono::seconds(3)) {
        std::unique_lock l(m_);
        if (!cv_.wait_for(l, t, [&] { return !in_.empty(); }))
            return {};
        auto s = std::move(in_.front());
        in_.pop_front();
        return s;
    }

private:
    static int callback(lws* wsi, lws_callback_reasons reason, void*, void* in, size_t len) {
        auto* self = static_cast<WsClient*>(lws_context_user(lws_get_context(wsi)));
        if (!self)
            return 0;
        switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED: {
            std::lock_guard l(self->m_);
            self->connected_ = true;
            self->cv_.notify_all();
            break;
        }
        case LWS_CALLBACK_CLIENT_RECEIVE: {
            std::lock_guard l(self->m_);
            self->partial_.append(static_cast<const char*>(in), len);
            if (lws_is_final_fragment(wsi)) {
                self->in_.push_back(std::move(self->partial_));
                self->partial_.clear();
                self->cv_.notify_all();
            }
            break;
        }
        case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
            if (self->wsi_) {
                std::lock_guard l(self->m_);
                if (!self->out_.empty())
                    lws_callback_on_writable(self->wsi_);
            }
            break;
        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            std::string text;
            {
                std::lock_guard l(self->m_);
                if (self->out_.empty())
                    break;
                text = std::move(self->out_.front());
                self->out_.pop_front();
            }
            std::vector<unsigned char> buf(LWS_PRE + text.size());
            std::copy(text.begin(), text.end(), buf.begin() + LWS_PRE);
            lws_write(wsi, buf.data() + LWS_PRE, text.size(), LWS_WRITE_TEXT);
            std::lock_guard l(self->m_);
            if (!self->out_.empty())
                lws_callback_on_writable(wsi);
            break;
        }
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        case LWS_CALLBACK_CLIENT_CLOSED: {
            std::lock_guard l(self->m_);
            self->closed_ = true;
            self->connected_ = false;
            self->wsi_ = nullptr;
            self->cv_.notify_all();
            break;
        }
        default:
            break;
        }
        return 0;
    }

    static const lws_protocols* protocols() {
        static const lws_protocols p[] = {{"test-client", callback, 0, 0, 0, nullptr, 0}, LWS_PROTOCOL_LIST_TERM};
        return p;
    }

    lws_context* context_ = nullptr;
    lws* wsi_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex m_;
    std::condition_variable cv_;
    bool connected_ = false;
    bool closed_ = false;
    std::deque<std::string> out_;
    std::deque<std::string> in_;
    std::string partial_;
};

}  // namespace tcng::test
