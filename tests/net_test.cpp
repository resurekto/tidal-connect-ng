#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <thread>

#include "net/Loop.hpp"
#include "net/UnixServer.hpp"
#include "test_util/Unique.hpp"
#include "test_util/UnixClient.hpp"

using namespace std::chrono_literals;
using namespace tcng::net;

TEST_CASE("Loop runs posted work on its own thread") {
    Loop loop;
    loop.start();
    std::promise<std::thread::id> where;
    loop.post([&] { where.set_value(std::this_thread::get_id()); });
    auto f = where.get_future();
    REQUIRE(f.wait_for(2s) == std::future_status::ready);
    CHECK(f.get() != std::this_thread::get_id());
    loop.stop();
}

TEST_CASE("Loop timers fire once and can be cancelled") {
    Loop loop;
    loop.start();
    std::atomic<int> fired{0}, cancelled{0};
    loop.schedule(20ms, [&] { ++fired; });
    auto id = loop.schedule(50ms, [&] { ++cancelled; });
    loop.cancel(id);
    std::this_thread::sleep_for(200ms);
    CHECK(fired == 1);
    CHECK(cancelled == 0);
    loop.stop();
}

TEST_CASE("UnixServer exchanges bytes with a client") {
    const auto path = tcng::test::uniqueSocketPath("tcng-net");

    Loop loop;
    std::mutex m;
    std::condition_variable cv;
    std::string received;
    ConnectionId peer = 0;
    bool closed = false;

    UnixServer* serverPtr = nullptr;
    UnixServer server(loop, {
        .onOpen = [&](ConnectionId id) { std::lock_guard l(m); peer = id; cv.notify_all(); },
        .onData = [&](ConnectionId id, std::string bytes) {
            {
                std::lock_guard l(m);
                received += bytes;
                cv.notify_all();
            }
            serverPtr->send(id, "pong:" + bytes);
        },
        .onClose = [&](ConnectionId) { std::lock_guard l(m); closed = true; cv.notify_all(); },
    });
    serverPtr = &server;
    server.listen(path);
    loop.start();

    tcng::test::UnixClient client(path);
    REQUIRE(client.connected());
    client.write("ping");
    CHECK(client.readExactly(9, 2s) == "pong:ping");

    {
        std::unique_lock l(m);
        REQUIRE(cv.wait_for(l, 2s, [&] { return peer != 0 && received == "ping"; }));
    }
    client.close();
    {
        std::unique_lock l(m);
        CHECK(cv.wait_for(l, 2s, [&] { return closed; }));
    }
    server.close();
    loop.stop();
    CHECK_FALSE(std::filesystem::exists(path));
}
