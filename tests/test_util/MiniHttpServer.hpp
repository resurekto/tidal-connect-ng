#pragma once
// A tiny HTTP/1.0 server for tests: serves fixed bodies by path on 127.0.0.1 (ephemeral
// port), one request per connection, "Connection: close". "Range: bytes=a-b" gets a 206 with
// Content-Range. Optional per-response delay.

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using TestSocket = SOCKET;
inline void closeTestSocket(TestSocket s) { closesocket(s); }
constexpr TestSocket kBadTestSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using TestSocket = int;
inline void closeTestSocket(TestSocket s) { ::close(s); }
constexpr TestSocket kBadTestSocket = -1;
#endif

namespace tcng::test {

class MiniHttpServer {
public:
    MiniHttpServer() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listen_, 16);
        socklen_t len = sizeof(addr);
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { loop(); });
    }
    ~MiniHttpServer() {
        running_ = false;
        thread_.join();
        closeTestSocket(listen_);
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void serve(const std::string& path, std::string body) {
        std::lock_guard l(m_);
        bodies_[path] = std::move(body);
    }
    void setDelay(std::chrono::milliseconds delay) { delayMs_ = delay.count(); }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }

private:
    void loop() {
        while (running_) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listen_, &set);
            timeval tv{0, 50 * 1000};
            if (::select(static_cast<int>(listen_) + 1, &set, nullptr, nullptr, &tv) <= 0)
                continue;
            const TestSocket c = ::accept(listen_, nullptr, nullptr);
            if (c == kBadTestSocket)
                continue;
            handle(c);
            closeTestSocket(c);
        }
    }

    void handle(TestSocket c) {
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos) {
            const auto n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0)
                return;
            req.append(buf, static_cast<std::size_t>(n));
        }
        const auto sp1 = req.find(' ');
        const auto sp2 = req.find(' ', sp1 + 1);
        const std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);
        std::string body;
        bool found = false;
        {
            std::lock_guard l(m_);
            if (auto it = bodies_.find(path); it != bodies_.end()) {
                body = it->second;
                found = true;
            }
        }
        if (delayMs_ > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_.load()));
        std::string status = found ? "200 OK" : "404 Not Found";
        std::string extra;
        std::string lower = req;
        for (auto& ch : lower)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (const auto r = lower.find("\r\nrange: bytes="); found && r != std::string::npos) {
            const auto from = r + 15;
            const auto dash = lower.find('-', from);
            const auto stop = lower.find("\r\n", from);
            const std::size_t total = body.size();
            const std::size_t a = std::stoull(lower.substr(from, dash - from));
            std::size_t b = dash + 1 < stop ? std::stoull(lower.substr(dash + 1, stop - dash - 1)) : total - 1;
            if (b >= total)
                b = total - 1;
            body = a < total ? body.substr(a, b - a + 1) : std::string();
            status = "206 Partial Content";
            extra = "\r\nContent-Range: bytes " + std::to_string(a) + "-" + std::to_string(b) + "/" + std::to_string(total);
        }
        const std::string head = "HTTP/1.0 " + status + extra + "\r\nContent-Length: " + std::to_string(body.size()) +
                                 "\r\nConnection: close\r\n\r\n";
        sendAll(c, head);
        sendAll(c, body);
    }

    static void sendAll(TestSocket c, const std::string& data) {
        std::size_t done = 0;
        while (done < data.size()) {
            const auto n = ::send(c, data.data() + done, static_cast<int>(data.size() - done), 0);
            if (n <= 0)
                return;
            done += static_cast<std::size_t>(n);
        }
    }

    TestSocket listen_ = kBadTestSocket;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{true};
    std::atomic<long long> delayMs_{0};
    std::mutex m_;
    std::map<std::string, std::string> bodies_;
    std::thread thread_;
};

}  // namespace tcng::test
