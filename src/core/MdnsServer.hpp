#pragma once
// The "mdns" service: advertises _tidalconnect._tcp via the DNS-SD API (Bonjour on
// macOS/Windows, avahi-compat on Linux). Results are processed on a dedicated thread.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "core/MessageCore.hpp"

typedef struct _DNSServiceRef_t* DNSServiceRef;

namespace tcng::core {

class MdnsServer : public core::Service {
public:
    struct Config {
        bool autoStart = true;
        std::string serviceType = "_tidalconnect._tcp";
        std::string modelName;
        std::uint32_t capabilities = 0;
        std::uint16_t port = 0;
        std::string deviceId;
        std::string friendlyName;
    };

    MdnsServer();
    ~MdnsServer() override;

    // Validates and applies the config; the instance name becomes "<model>-<deviceId>".
    bool setConfig(const Config& config);

    void start() override;
    void finish() override;

    // Exposed for tests: the TXT record entries, in order.
    static std::string txtRecord(const Config& config);

protected:
    void handleMessage(std::uint32_t id, const core::Json& payload) override;

private:
    enum class State : int { Stopped = 0, Running = 1, StartFailed = 2 };

    static bool verifyConfig(const Config& config);
    bool startServer();
    void stopServer();
    bool changeFriendlyName();
    void pollLoop();

    void procSetConfig(const core::Json& j);
    void procStartServer();
    void procStopServer();
    void procChangeFriendlyName(const core::Json& j);

    Config config_;
    std::string serviceName_;
    State state_ = State::Stopped;  // mdns thread only

    std::mutex refMutex_;  // guards ref_ against the poll thread
    DNSServiceRef ref_ = nullptr;
    std::thread pollThread_;
    std::atomic<bool> pollStop_{false};
};

}  // namespace tcng::core
