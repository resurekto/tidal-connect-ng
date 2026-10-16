add_subdirectory(util)
add_subdirectory(media)

# All of TCNG except main() lives in one static library, so tests can link it.
add_library(tcng STATIC
    CommandLine.cpp
    TidalConnect.cpp
    core/ClientCommunicator.cpp
    core/DeviceInfo.cpp
    core/MdnsServer.cpp
    core/MessageCore.cpp
    core/PlaybackController.cpp
    core/SessionManager.cpp
    net/Loop.cpp
    net/UnixServer.cpp
    net/WebsocketServer.cpp
    queue/QueueItem.cpp
    queue/QueueManager.cpp
    queue/QueueWindow.cpp
    queue/QueueWindowLoader.cpp
    spkcon/SpeakerCommunicator.cpp
    spkcon/SpkconPacket.cpp
    spkcon/SpkconServer.cpp)

target_include_directories(tcng PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tcng PUBLIC
    tcng-media tcng-util
    nlohmann_json::nlohmann_json fmt::fmt spdlog::spdlog CLI11::CLI11
    libwebsockets::websockets portaudio
    OpenSSL::SSL OpenSSL::Crypto CURL::libcurl DNSSD::DNSSD
    PkgConfig::FLAC PkgConfig::AVCODEC PkgConfig::AVUTIL Threads::Threads)
if(APPLE)
    target_link_libraries(tcng PUBLIC "-framework IOKit" "-framework CoreFoundation")
endif()
if(WIN32)
    target_link_libraries(tcng PUBLIC ws2_32 iphlpapi advapi32)
    target_compile_definitions(tcng PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
endif()

if(MSVC)
    target_compile_options(tcng PRIVATE /W4 /permissive-)
else()
    target_compile_options(tcng PRIVATE -Wall -Wextra)
endif()

add_executable(tidal-connect-ng main.cpp)
target_link_libraries(tidal-connect-ng PRIVATE tcng)
target_compile_definitions(tidal-connect-ng PRIVATE TCNG_VERSION="${PROJECT_VERSION}")
