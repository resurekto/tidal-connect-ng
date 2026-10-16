#include "core/DeviceInfo.hpp"

#include <array>
#include <cstdio>
#include <fstream>

#include <openssl/evp.h>

#include "util/Log.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <iphlpapi.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <ifaddrs.h>
#include <net/if_dl.h>
#include <sys/socket.h>
#else
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/socket.h>
#endif

namespace tcng::core {

std::string DeviceInfo::md5Hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(data.data(), data.size(), digest, &len, EVP_md5(), nullptr);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0xF]);
    }
    return out;
}

std::string DeviceInfo::formatMac(const std::uint8_t (&mac)[6]) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                  mac[5]);
    return buf;
}

std::optional<std::string> DeviceInfo::macAddress(const std::string& netif) {
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::string buffer(size, '\0');
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, addrs, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        rc = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, addrs, &size);
    }
    if (rc != NO_ERROR)
        return std::nullopt;
    for (auto* a = addrs; a; a = a->Next) {
        // Match the adapter GUID name or the friendly name ("Ethernet", "Wi-Fi").
        std::string friendly;
        if (a->FriendlyName) {
            int n = WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) {
                friendly.resize(static_cast<std::size_t>(n - 1));
                WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, friendly.data(), n, nullptr, nullptr);
            }
        }
        if ((a->AdapterName && netif == a->AdapterName) || netif == friendly) {
            if (a->PhysicalAddressLength != 6)
                return std::nullopt;
            std::uint8_t mac[6];
            std::copy(a->PhysicalAddress, a->PhysicalAddress + 6, mac);
            return formatMac(mac);
        }
    }
    return std::nullopt;
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
        return std::nullopt;
    std::optional<std::string> result;
    for (auto* ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || netif != ifa->ifa_name)
            continue;
#if defined(__APPLE__)
        if (ifa->ifa_addr->sa_family != AF_LINK)
            continue;
        auto* sdl = reinterpret_cast<const sockaddr_dl*>(ifa->ifa_addr);
        if (sdl->sdl_alen != 6)
            continue;
        std::uint8_t mac[6];
        std::copy_n(reinterpret_cast<const std::uint8_t*>(LLADDR(sdl)), 6, mac);
#else
        if (ifa->ifa_addr->sa_family != AF_PACKET)
            continue;
        auto* sll = reinterpret_cast<const sockaddr_ll*>(ifa->ifa_addr);
        if (sll->sll_halen != 6)
            continue;
        std::uint8_t mac[6];
        std::copy_n(sll->sll_addr, 6, mac);
#endif
        result = formatMac(mac);
        break;
    }
    freeifaddrs(list);
    return result;
#endif
}

std::optional<std::string> DeviceInfo::machineId() {
#if defined(_WIN32)
    wchar_t value[64];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    char utf8[128];
    int n = WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (n <= 1)
        return std::nullopt;
    return std::string(utf8, static_cast<std::size_t>(n - 1));
#elif defined(__APPLE__)
    io_service_t platform =
        IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
    if (!platform)
        return std::nullopt;
    std::optional<std::string> result;
    if (CFTypeRef uuid = IORegistryEntryCreateCFProperty(platform, CFSTR(kIOPlatformUUIDKey), kCFAllocatorDefault, 0)) {
        char buf[128];
        if (CFGetTypeID(uuid) == CFStringGetTypeID() &&
            CFStringGetCString(static_cast<CFStringRef>(uuid), buf, sizeof(buf), kCFStringEncodingUTF8))
            result = buf;
        CFRelease(uuid);
    }
    IOObjectRelease(platform);
    return result;
#else
    for (const char* path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
        std::ifstream in(path);
        std::string id;
        if (in && std::getline(in, id) && !id.empty())
            return id;
    }
    return std::nullopt;
#endif
}

std::string DeviceInfo::generateDeviceId(const std::string& netif) {
    auto mac = macAddress(netif);
    if (!mac) {
        log::get()->critical("cannot read the MAC address of network interface \"{}\"", netif);
        return ILLEGAL_HASH_VALUE;
    }
    log::get()->debug("mac addr : \"{}\"", *mac);
    auto id = md5Hex("tisoc" + *mac + "cosit");
    log::get()->debug("device id: \"{}\"", id);
    return id;
}

std::string DeviceInfo::generateDeviceIdFromMachineId() {
    auto id = machineId();
    if (!id) {
        log::get()->critical("cannot read the machine id");
        return ILLEGAL_HASH_VALUE;
    }
    return md5Hex("tisoc" + *id + "cosit");
}

std::string DeviceInfo::deviceId(const std::string& netif, const std::string& additionalSeed) {
    const std::string base = netif.empty() ? generateDeviceIdFromMachineId() : generateDeviceId(netif);
    return md5Hex(base + additionalSeed);
}

}  // namespace tcng::core
