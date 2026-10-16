add_executable(tcng-media-tests
    buffer_test.cpp
    engine_test.cpp
    flac_decoder_test.cpp
    media_player_test.cpp
    python_tidal_test.cpp
    segment_stream_test.cpp
    stream_test.cpp
    thread_pool_test.cpp
    tidal_url_fetch_test.cpp
    volume_test.cpp)
target_link_libraries(tcng-media-tests PRIVATE tcng-media Catch2::Catch2WithMain)
target_include_directories(tcng-media-tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
if(WIN32)
    target_link_libraries(tcng-media-tests PRIVATE ws2_32)
endif()
if(NOT MSVC)
    # GCC < 13 flags designated initializers that leave fields out.
    target_compile_options(tcng-media-tests PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(tcng-media-tests)
