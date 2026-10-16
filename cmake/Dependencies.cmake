# Third-party dependencies. Header-only and small libraries come from FetchContent,
# pinned to exact releases; platform libraries (TLS, HTTP, codecs, mDNS) are found on
# the system. Linux, macOS and Windows are all supported.

include(FetchContent)
find_package(PkgConfig)

# --- System ----------------------------------------------------------------

find_package(Threads REQUIRED)
find_package(OpenSSL 3 REQUIRED)
find_package(CURL REQUIRED)
find_package(DNSSD REQUIRED)

if(PkgConfig_FOUND)
    pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac)
    pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
    pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
    # Media pipeline: FLAC++ (FLACDecoder is a FLAC::Decoder::Stream),
    # avformat (MP4 demux) and swresample (to S16 stereo) for the AAC decoder.
    pkg_check_modules(FLACPP REQUIRED IMPORTED_TARGET flac++)
    pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
    pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
    # --volume mixer on Linux: the ALSA mixer (PortAudio needs ALSA there anyway).
    if(UNIX AND NOT APPLE)
        pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
    endif()
else()
    message(FATAL_ERROR "pkg-config is required to locate libFLAC and FFmpeg")
endif()

# --- FetchContent ----------------------------------------------------------

set(JSON_Install OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE)

set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt
    GIT_TAG        12.2.0
    GIT_SHALLOW    TRUE)

set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE)

set(CLI11_PRECOMPILED OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11
    GIT_TAG        v2.7.2
    GIT_SHALLOW    TRUE)

# libwebsockets: static, TLS via the system OpenSSL 3, HTTP/1.1 only, Unix domain sockets for the spkcon channel.
set(LWS_WITH_SSL ON CACHE BOOL "" FORCE)
# OpenSSL on every platform: HTTP/3 would switch lws to GnuTLS, Windows would default
# to SChannel.
set(LWS_WITH_HTTP3 OFF CACHE BOOL "" FORCE)
set(LWS_WITH_GNUTLS OFF CACHE BOOL "" FORCE)
set(LWS_WITH_SCHANNEL OFF CACHE BOOL "" FORCE)
set(LWS_WITH_SECURE_STREAMS OFF CACHE BOOL "" FORCE)
set(LWS_WITH_CONMON OFF CACHE BOOL "" FORCE)
set(LWS_WITH_STATIC ON CACHE BOOL "" FORCE)
set(LWS_WITH_SHARED OFF CACHE BOOL "" FORCE)
set(LWS_WITH_HTTP2 OFF CACHE BOOL "" FORCE)
set(LWS_UNIX_SOCK ON CACHE BOOL "" FORCE)
set(LWS_IPV6 ON CACHE BOOL "" FORCE)
set(LWS_WITH_MINIMAL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TESTAPPS ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_SERVER ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_SERVER_EXTPOLL ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_PING ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_CLIENT ON CACHE BOOL "" FORCE)
set(LWS_WITH_PLUGINS OFF CACHE BOOL "" FORCE)
set(LWS_WITH_EVLIB_PLUGINS OFF CACHE BOOL "" FORCE)
set(LWS_WITH_LIBUV OFF CACHE BOOL "" FORCE)
set(LWS_WITH_LIBEVENT OFF CACHE BOOL "" FORCE)
set(LWS_WITH_LIBEV OFF CACHE BOOL "" FORCE)
set(LWS_WITH_GLIB OFF CACHE BOOL "" FORCE)
set(LWS_INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(DISABLE_WERROR ON CACHE BOOL "" FORCE)
set(LWS_OPENSSL_INCLUDE_DIRS ${OPENSSL_INCLUDE_DIR} CACHE PATH "" FORCE)
set(LWS_OPENSSL_LIBRARIES "${OPENSSL_SSL_LIBRARY};${OPENSSL_CRYPTO_LIBRARY}" CACHE STRING "" FORCE)
# Newer macOS SDKs declare pipe2() (weak) while older running systems lack it, so the
# probe passes and lws calls a null pointer. Pipe() is fine on Apple; skip the probe.
if(APPLE)
    set(LWS_HAVE_PIPE2 "" CACHE INTERNAL "")
endif()
FetchContent_Declare(libwebsockets
    GIT_REPOSITORY https://github.com/warmcat/libwebsockets
    GIT_TAG        v5.0.0
    GIT_SHALLOW    TRUE)

# PortAudio: the last release (v19.7.0) is from 2021, so pin a master commit.
# Host APIs are auto-detected per platform (ALSA/PulseAudio/JACK, CoreAudio, WASAPI).
set(PA_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(PA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# Fetched as a commit tarball: CMake 3.25's git step can skip the clone of a commit pin.
FetchContent_Declare(portaudio
    URL      https://github.com/PortAudio/portaudio/archive/d71746635dae088c5d7c808d5f29f0b939458739.tar.gz
    URL_HASH SHA256=f756f0a6020f547190ed6960cf72e809ec73fc659d91f94da8cac521896c502f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(nlohmann_json fmt spdlog CLI11 libwebsockets portaudio)

# Treat third-party headers as system headers in our targets (no warnings from them).
# (FetchContent_Declare(... SYSTEM) is not parsed by CMake 3.25, Debian 12's version.)
foreach(dep nlohmann_json fmt spdlog CLI11 websockets portaudio)
    if(TARGET ${dep})
        set_property(TARGET ${dep} PROPERTY SYSTEM TRUE)
    endif()
endforeach()

# Third-party C sources: their warnings are not ours to fix.
if(MSVC)
    target_compile_options(portaudio PRIVATE /w)
else()
    target_compile_options(portaudio PRIVATE -w)
endif()

# libwebsockets' static target already carries its include directories.
if(NOT TARGET libwebsockets::websockets)
    add_library(libwebsockets::websockets ALIAS websockets)
endif()

if(TCNG_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2
        GIT_TAG        v3.16.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)
endif()
