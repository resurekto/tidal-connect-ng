#pragma once
// Per-process ports and socket paths, so test cases can run in parallel (ctest -j).

#include <cstdint>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#define TCNG_GETPID _getpid
#else
#include <unistd.h>
#define TCNG_GETPID getpid
#endif

namespace tcng::test {

inline std::uint16_t uniquePort(unsigned offset = 0) {
    return static_cast<std::uint16_t>(20000 + (static_cast<unsigned>(TCNG_GETPID()) * 7u + offset) % 30000u);
}

inline std::string uniqueSocketPath(const std::string& name) {
    // Kept short: Unix socket paths are limited to ~104 bytes.
    return (std::filesystem::temp_directory_path() / (name + "-" + std::to_string(TCNG_GETPID()) + ".sock")).string();
}

}  // namespace tcng::test
