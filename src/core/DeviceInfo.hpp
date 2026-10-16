#pragma once
// The device id, as advertised over mDNS and to the apps:
//
//   base     = md5hex("tisoc" + source + "cosit")
//   deviceId = md5hex(base + additionalSeed)          (32 lowercase hex chars)
//
// source is the MAC of --netif-for-deviceid ("aa:bb:cc:dd:ee:ff"), or — when no
// interface is given — the OS machine id (/etc/machine-id, macOS IOPlatformUUID, Windows
// MachineGuid).

#include <cstdint>
#include <optional>
#include <string>

namespace tcng::core {

class DeviceInfo {
public:
    static constexpr std::uint32_t CAP_CODEC_MPEGH = 1;        // never advertised by TCNG
    static constexpr std::uint32_t CAP_CODEC_MQA = 2;          // never advertised by TCNG
    static constexpr std::uint32_t CAP_CODEC_DOLBY_ATMOS = 4;  // never advertised by TCNG
    static inline const std::string ILLEGAL_HASH_VALUE = "XXXXXXXXXXXXXXXX";

    // md5hex("tisoc" + mac + "cosit") for the named interface, or ILLEGAL_HASH_VALUE.
    static std::string generateDeviceId(const std::string& netif);
    // md5hex("tisoc" + machineId + "cosit"), or ILLEGAL_HASH_VALUE.
    static std::string generateDeviceIdFromMachineId();
    // Full device id from the CLI settings.
    static std::string deviceId(const std::string& netif, const std::string& additionalSeed);

    // Building blocks (exposed for tests).
    static std::string md5Hex(const std::string& data);
    static std::string formatMac(const std::uint8_t (&mac)[6]);
    static std::optional<std::string> macAddress(const std::string& netif);
    static std::optional<std::string> machineId();
};

}  // namespace tcng::core
