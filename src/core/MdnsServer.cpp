#include "core/MdnsServer.hpp"

#include <dns_sd.h>

#include <chrono>
#include <cstdlib>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#endif

#include "util/JsonUtil.hpp"
#include "core/MessageIds.hpp"
#include "util/Log.hpp"

namespace tcng::core {

namespace {

constexpr std::size_t kMaxFieldLength = 256;
constexpr int kResultSuccess = 0;
constexpr int kResultSetConfigFailed = 0x448;
constexpr int kResultTxtUpdateFailed = 0x44a;

void addTxt(TXTRecordRef& txt, const char* key, const std::string& value) {
    TXTRecordSetValue(&txt, key, static_cast<uint8_t>(std::min<std::size_t>(value.size(), 255)), value.data());
}

void DNSSD_API registerReply(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType error, const char* name,
                             const char* type, const char*, void*) {
    if (error == kDNSServiceErr_NoError)
        log::get()->debug("mdns service registered: {}.{}", name, type);
    else if (error == kDNSServiceErr_NameConflict)
        log::get()->error("mdns name collision: {}", name);
    else
        log::get()->error("mdns registration failed: {}", error);
}

}  // namespace

MdnsServer::MdnsServer() : Service(core::service::mdns) {
#if !defined(__APPLE__) && !defined(_WIN32)
    // avahi-compat prints a "use the native API" banner on every call otherwise.
    setenv("AVAHI_COMPAT_NOWARN", "1", 0);
#endif
}

MdnsServer::~MdnsServer() {
    finish();
}

bool MdnsServer::verifyConfig(const Config& c) {
    auto check = [](const char* field, const std::string& v) {
        if (v.size() < kMaxFieldLength)
            return true;
        log::get()->error("setConfig: invalid config.{}: {}", field, v.size());
        return false;
    };
    return check("serviceType", c.serviceType) && check("modelName", c.modelName) &&
           check("friendlyName", c.friendlyName) && check("serviceName", c.modelName + "-" + c.deviceId);
}

bool MdnsServer::setConfig(const Config& config) {
    if (!verifyConfig(config))
        return false;
    config_ = config;
    serviceName_ = config_.modelName + "-" + config_.deviceId;
    log::get()->trace("serviceName : {}", serviceName_);
    return true;
}

std::string MdnsServer::txtRecord(const Config& c) {
    TXTRecordRef txt;
    TXTRecordCreate(&txt, 0, nullptr);
    addTxt(txt, "mn", c.modelName);
    addTxt(txt, "ca", std::to_string(c.capabilities));
    addTxt(txt, "id", c.deviceId);
    addTxt(txt, "fn", c.friendlyName);
    addTxt(txt, "ve", "1");
    std::string out(static_cast<const char*>(TXTRecordGetBytesPtr(&txt)), TXTRecordGetLength(&txt));
    TXTRecordDeallocate(&txt);
    return out;
}

bool MdnsServer::startServer() {
    std::lock_guard lock(refMutex_);
    if (ref_)
        return true;
    const std::string txt = txtRecord(config_);
    DNSServiceRef ref = nullptr;
    // Default flags: avahi's compatibility layer rejects kDNSServiceFlagsNoAutoRename
    // (kDNSServiceErr_Unsupported), so a name collision gets the usual " (2)" rename.
    const auto err = DNSServiceRegister(&ref, 0, kDNSServiceInterfaceIndexAny,
                                        serviceName_.c_str(), config_.serviceType.c_str(), nullptr, nullptr,
                                        htons(config_.port), static_cast<uint16_t>(txt.size()), txt.data(),
                                        registerReply, this);
    if (err != kDNSServiceErr_NoError) {
        log::get()->error("DNSServiceRegister() FAILED: {}", err);
        return false;
    }
    ref_ = ref;
    pollStop_ = false;
    pollThread_ = std::thread([this] { pollLoop(); });
    log::get()->debug("mdns service started!");
    return true;
}

void MdnsServer::stopServer() {
    pollStop_ = true;
    if (pollThread_.joinable())
        pollThread_.join();
    std::lock_guard lock(refMutex_);
    if (ref_) {
        // Deallocating withdraws the records (goodbye packets).
        DNSServiceRefDeallocate(ref_);
        ref_ = nullptr;
    }
}

void MdnsServer::pollLoop() {
    while (!pollStop_) {
        std::unique_lock lock(refMutex_);
        if (!ref_)
            return;
        const auto fd = DNSServiceRefSockFD(ref_);
        lock.unlock();

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        timeval tv{0, 100 * 1000};
        const int n = select(static_cast<int>(fd) + 1, &readable, nullptr, nullptr, &tv);
        if (n <= 0 || pollStop_)
            continue;
        lock.lock();
        if (ref_ && DNSServiceProcessResult(ref_) != kDNSServiceErr_NoError) {
            log::get()->error("DNSServiceProcessResult failed; mdns poll stopped");
            return;
        }
    }
}

bool MdnsServer::changeFriendlyName() {
    std::lock_guard lock(refMutex_);
    if (!ref_)
        return false;
    const std::string txt = txtRecord(config_);
    return DNSServiceUpdateRecord(ref_, nullptr, 0, static_cast<uint16_t>(txt.size()), txt.data(), 0) ==
           kDNSServiceErr_NoError;
}

void MdnsServer::start() {
    log::get()->debug("config.autoStart: {}", config_.autoStart);
    if (config_.autoStart && state_ == State::Stopped) {
        if (startServer()) {
            state_ = State::Running;
        } else {
            log::get()->error("avahi start failed.");
            state_ = State::StartFailed;
        }
    }
    Service::start();
}

void MdnsServer::finish() {
    Service::finish();
    if (state_ != State::Stopped) {
        state_ = State::Stopped;
        stopServer();
    }
    log::get()->debug("MdnsServer::finish()");
}

void MdnsServer::handleMessage(std::uint32_t id, const Json& j) {
    switch (id) {
    case core::msg::MdnsSetConfig: procSetConfig(j); break;
    case core::msg::MdnsStartServer: procStartServer(); break;
    case core::msg::MdnsStopServer: procStopServer(); break;
    case core::msg::MdnsChangeFriendlyName: procChangeFriendlyName(j); break;
    default: log::get()->error("[avahi handlMessage Error]unknown command:0x{:x}", id); break;
    }
}

void MdnsServer::procSetConfig(const Json& j) {
    if (state_ != State::Stopped) {
        log::get()->error("avahi server already started.");
        return;
    }
    Config c = config_;
    const Json& packet = jv::at(j, "packet");
    if (auto v = jv::string(packet, "modelName"); !v.empty())
        c.modelName = v;
    if (auto v = jv::string(packet, "friendlyName"); !v.empty())
        c.friendlyName = v;
    if (!setConfig(c))
        log::get()->error("avahi setConfig failed.");
}

void MdnsServer::procStartServer() {
    if (state_ != State::Stopped)
        return;
    state_ = startServer() ? State::Running : State::StartFailed;
}

void MdnsServer::procStopServer() {
    if (state_ == State::Stopped)
        return;
    state_ = State::Stopped;
    stopServer();
}

void MdnsServer::procChangeFriendlyName(const Json& j) {
    const Json& packet = jv::at(j, "packet");
    const int requestId = jv::integer(packet, "requestId");
    Config c = config_;
    c.friendlyName = jv::string(packet, "friendlyName");
    int result = kResultSuccess;
    if (!setConfig(c))
        result = kResultSetConfigFailed;
    else if (state_ == State::Running && !changeFriendlyName())
        result = kResultTxtUpdateFailed;
    sendMessage(core::service::speaker, core::msg::SendPacket,
                {{"socketId", 0},
                 {"packet", {{"command", "notifyRequestResult"}, {"requestId", requestId}, {"resultCode", result}}}});
}

}  // namespace tcng::core
