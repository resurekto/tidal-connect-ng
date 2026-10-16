add_executable(tcng-tests
    app_test.cpp
    clientcomm_test.cpp
    command_line_test.cpp
    device_info_test.cpp
    mdns_test.cpp
    message_core_test.cpp
    net_test.cpp
    spkcon_packet_test.cpp
    queue_test.cpp
    session_test.cpp
    speaker_test.cpp)
target_include_directories(tcng-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tcng-tests PRIVATE tcng Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(tcng-tests)
add_subdirectory(media)
