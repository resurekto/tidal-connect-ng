#pragma once
// Minimal blocking AF_UNIX client for tests (POSIX and Windows 10+).

#include <chrono>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <afunix.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <cstring>

namespace tcng::test {

class UnixClient {
public:
    explicit UnixClient(const std::string& path) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }
    ~UnixClient() { close(); }

    bool connected() const { return connected_; }

    void write(const std::string& bytes) {
        ::send(fd_, bytes.data(), static_cast<int>(bytes.size()), 0);
    }

    std::string readExactly(std::size_t n, std::chrono::milliseconds timeout) {
        std::string out;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (out.size() < n && std::chrono::steady_clock::now() < deadline) {
            pollfd p{};
            p.fd = fd_;
            p.events = POLLIN;
#ifdef _WIN32
            if (WSAPoll(&p, 1, 50) <= 0)
                continue;
#else
            if (::poll(&p, 1, 50) <= 0)
                continue;
#endif
            char buf[4096];
            const auto r = ::recv(fd_, buf, static_cast<int>(std::min(sizeof(buf), n - out.size())), 0);
            if (r <= 0)
                break;
            out.append(buf, static_cast<std::size_t>(r));
        }
        return out;
    }

    void close() {
        if (fd_ == kInvalid)
            return;
#ifdef _WIN32
        ::closesocket(fd_);
#else
        ::close(fd_);
#endif
        fd_ = kInvalid;
    }

private:
#ifdef _WIN32
    using Fd = SOCKET;
    static constexpr Fd kInvalid = INVALID_SOCKET;
#else
    using Fd = int;
    static constexpr Fd kInvalid = -1;
#endif
    Fd fd_ = kInvalid;
    bool connected_ = false;
};

}  // namespace tcng::test
