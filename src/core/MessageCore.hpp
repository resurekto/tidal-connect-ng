#pragma once
// Service framework. Each service owns a
// thread that drains an unbounded FIFO of {id, sender, json} messages one at a time;
// services talk to each other only by posting messages by name.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace tcng::core {

using Json = nlohmann::json;

struct Message {
    std::uint32_t id = 0;
    std::string sender;  // name of the sending service
    Json payload;
};

class IMessageSender {
public:
    virtual ~IMessageSender() = default;
    virtual void sendMessage(const std::string& dest, std::unique_ptr<const Message> msg) = 0;
};

class MessageCore {
public:
    explicit MessageCore(std::string name);
    virtual ~MessageCore();

    const std::string& getName() const { return name_; }
    void setMessageSender(const std::shared_ptr<IMessageSender>& sender);

    void sendMessage(const std::string& dest, std::uint32_t id, const Json& payload);
    void sendMessage(const std::vector<std::string>& dests, std::uint32_t id, const Json& payload);
    void sendMessage(const std::string& dest, std::unique_ptr<const Message> msg);

    // Queues a message for this service. Thread-safe.
    void postMessage(std::unique_ptr<const Message> msg);
    // Self-post: sender is this service.
    void postMessage(std::uint32_t id, const Json& payload);

protected:
    virtual void handleMessage(std::uint32_t id, const Json& payload);

    // Runs loop() on a new thread / stops and joins it. Pending messages are dropped.
    void startLoop();
    void stopLoop();
    bool loopRunning() const { return thread_.joinable(); }

private:
    void loop();
    void clearQueue();

    std::string name_;
    std::weak_ptr<IMessageSender> sender_;
    std::thread thread_;
    std::deque<std::unique_ptr<const Message>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

enum class ServiceState : int { Stopped = 0, Started = 1 };

class IServiceManager {
public:
    virtual ~IServiceManager() = default;
    virtual void startAll() = 0;
    virtual void finishAllOthers(const std::string& name) = 0;
};

class Service : public MessageCore {
public:
    using MessageCore::MessageCore;

    // Start with access to the manager ("activator" service). Default: start().
    virtual bool start(const std::shared_ptr<IServiceManager>& manager);
    virtual void start();
    virtual void finish();
    ServiceState getServiceState() const { return state_.load(); }

protected:
    std::atomic<ServiceState> state_{ServiceState::Stopped};
};

class ServiceManager : public IServiceManager, public std::enable_shared_from_this<ServiceManager> {
public:
    ServiceManager();

    void registerService(const std::shared_ptr<Service>& service);
    void start(const std::string& name);
    void startAll() override;
    void finishAll();
    void finishAllOthers(const std::string& name) override;

private:
    class Sender;
    // Services start in reverse registration order.
    std::vector<std::shared_ptr<Service>> inStartOrder() const;

    std::shared_ptr<Sender> sender_;
};

}  // namespace tcng::core
