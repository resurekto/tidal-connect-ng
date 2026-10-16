# Generic building blocks with no knowledge of TIDAL or the receiver: logging, JSON
# accessors, a process runner, a thread pool, a ring buffer and a libcurl HTTP client.
add_library(tcng-util STATIC
    AsyncHttpClient.cpp
    HttpClient.cpp
    Log.cpp
    RingBuffer.cpp
    Subprocess.cpp
    ThreadPool.cpp)
target_include_directories(tcng-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tcng-util PUBLIC
    nlohmann_json::nlohmann_json fmt::fmt spdlog::spdlog CURL::libcurl Threads::Threads)
if(MSVC)
    target_compile_options(tcng-util PRIVATE /W4 /permissive-)
else()
    target_compile_options(tcng-util PRIVATE -Wall -Wextra)
endif()
