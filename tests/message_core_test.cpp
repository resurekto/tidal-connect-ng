#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "core/MessageCore.hpp"

using namespace std::chrono_literals;
using namespace tcng::core;

namespace {

std::mutex gOrderMutex;
std::vector<std::string> gOrder;

class Echo : public Service {
public:
    explicit Echo(std::string name) : Service(std::move(name)) {}
    std::promise<std::pair<std::uint32_t, std::string>> got;

    void start() override {
        {
            std::lock_guard l(gOrderMutex);
            gOrder.push_back(getName());
        }
        Service::start();
    }

protected:
    void handleMessage(std::uint32_t id, const Json& payload) override {
        if (id == 1)
            sendMessage(payload.at("to").get<std::string>(), 2, {{"from", getName()}});
        else
            got.set_value({id, payload.at("from").get<std::string>()});
    }
};

}  // namespace

TEST_CASE("services exchange messages by name") {
    auto manager = std::make_shared<ServiceManager>();
    auto a = std::make_shared<Echo>("a");
    auto b = std::make_shared<Echo>("b");
    manager->registerService(a);
    manager->registerService(b);
    manager->startAll();

    a->postMessage(1, {{"to", "b"}});
    auto f = b->got.get_future();
    REQUIRE(f.wait_for(2s) == std::future_status::ready);
    CHECK(f.get() == std::pair<std::uint32_t, std::string>{2, "a"});
    manager->finishAll();
    CHECK(a->getServiceState() == ServiceState::Stopped);
}

TEST_CASE("startAll runs in reverse registration order and skips started ones") {
    gOrder.clear();
    auto manager = std::make_shared<ServiceManager>();
    for (const char* n : {"clientcomm", "queue", "playback", "session", "speaker", "mdns"})
        manager->registerService(std::make_shared<Echo>(n));
    manager->start("speaker");
    manager->startAll();
    CHECK(gOrder == std::vector<std::string>{"speaker", "mdns", "session", "playback", "queue", "clientcomm"});
    manager->finishAll();
}
