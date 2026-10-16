# Media pipeline: HTTP, TIDAL URL fetch, stream fetching, decoders, audio sink,
# playback engines.
add_library(tcng-media STATIC
    MediaPlayer.cpp
    PlaybackEngineGeneric.cpp
    PlaybackEngineTidal.cpp
    PythonTidalFetchTask.cpp
    TidalUrlFetchTask.cpp
    audio/AACDecoder.cpp
    audio/AudioDecoderFactory.cpp
    audio/AudioOutputBuffer.cpp
    audio/AudioPipeline.cpp
    audio/AudioWorker.cpp
    audio/FLACDecoder.cpp
    audio/Formats.cpp
    output/PortSink.cpp
    output/SoftwareGain.cpp
    output/VolumeControl.cpp
    output/VolumeCurve.cpp
    stream/BandwidthDetector.cpp
    stream/BufferStream.cpp
    stream/HttpStreamFetcher.cpp
    stream/Mp4Flac.cpp
    stream/Operation.cpp
    stream/Range.cpp
    stream/SegmentStreamManager.cpp
    stream/StreamManager.cpp)
# --volume mixer: one implementation per platform.
if(APPLE)
    target_sources(tcng-media PRIVATE output/CoreAudioVolume.cpp)
    target_link_libraries(tcng-media PUBLIC "-framework CoreAudio" "-framework CoreFoundation")
elseif(WIN32)
    target_sources(tcng-media PRIVATE output/WasapiVolume.cpp)
    target_link_libraries(tcng-media PUBLIC ole32)
else()
    target_sources(tcng-media PRIVATE output/AlsaMixerVolume.cpp)
    target_link_libraries(tcng-media PUBLIC PkgConfig::ALSA)
endif()
target_include_directories(tcng-media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# python-tidal-helper.py, embedded for PythonTidalFetchTask (fed to python on stdin) and
# --python-tidal-login.
set(TCNG_HELPER_PY ${CMAKE_CURRENT_SOURCE_DIR}/python-tidal-helper.py)
set(TCNG_HELPER_HPP ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded/PythonTidalHelper.hpp)
add_custom_command(OUTPUT ${TCNG_HELPER_HPP}
    COMMAND ${CMAKE_COMMAND} -DIN=${TCNG_HELPER_PY} -DOUT=${TCNG_HELPER_HPP} -DNAME=pythonTidalHelperScript
            -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${TCNG_HELPER_PY} ${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake
    COMMENT "Embedding python-tidal-helper.py")
target_sources(tcng-media PRIVATE ${TCNG_HELPER_HPP})
target_include_directories(tcng-media PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(tcng-media PUBLIC
    tcng-util
    nlohmann_json::nlohmann_json fmt::fmt spdlog::spdlog portaudio
    OpenSSL::SSL OpenSSL::Crypto CURL::libcurl
    PkgConfig::FLAC PkgConfig::FLACPP PkgConfig::AVFORMAT PkgConfig::AVCODEC PkgConfig::AVUTIL
    PkgConfig::SWRESAMPLE Threads::Threads)
if(MSVC)
    target_compile_options(tcng-media PRIVATE /W4 /permissive-)
else()
    target_compile_options(tcng-media PRIVATE -Wall -Wextra)
endif()
