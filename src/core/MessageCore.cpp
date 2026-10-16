#include "core/MessageCore.hpp"

#include <algorithm>
#include <map>

#include "util/Log.hpp"

namespace tcng::core {

// --- MessageCore -------------------------------------------------------------

MessageCore::MessageCore(std::string name) : name_(std::move(name)) {}

MessageCore::~MessageCore() {
    stopLoop();
}

void MessageCore::setMessageSender(const std::shared_ptr<IMessageSender>& sender) {
    sender_ = sender;
}

void MessageCore::sendMessage(const std::string& dest, std::unique_ptr<const Message> msg) {
    if (auto sender = sender_.lock())
        sender->sendMessage(dest, std::move(msg));
}

void MessageCore::sendMessage(const std::string& dest, std::uint32_t id, const Json& payload) {
    sendMessage(dest, std::make_unique<const Message>(Message{id, name_, payload}));
}

void MessageCore::sendMessage(const std::vector<std::string>& dests, std::uint32_t id, const Json& payload) {
    for (const auto& dest : dests)
        sendMessage(dest, id, payload);
}

void MessageCore::postMessage(std::unique_ptr<const Message> msg) {
    std::lock_guard lock(mutex_);
    log::get()->trace("msg send: {}->{},0x{:08x},{}", msg->sender, name_, msg->id, msg->payload.dump());
    queue_.push_back(std::move(msg));
    cv_.notify_one();
}

void MessageCore::postMessage(std::uint32_t id, const Json& payload) {
    postMessage(std::make_unique<const Message>(Message{id, name_, payload}));
}

void MessageCore::handleMessage(std::uint32_t id, const Json&) {
    log::get()->critical("[ASSERT] {}: unhandled message 0x{:08x}", name_, id);
}

void MessageCore::startLoop() {
    if (thread_.joinable())
        return;
    stop_ = false;
    thread_ = std::thread([this] { loop(); });
}

void MessageCore::stopLoop() {
    if (!thread_.joinable())
        return;
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        cv_.notify_one();
    }
    if (thread_.get_id() == std::this_thread::get_id())
        thread_.detach();  // finishing from our own handler; the loop exits after it returns
    else
        thread_.join();
    stop_ = false;
}

void MessageCore::loop() {
    for (;;) {
        std::unique_ptr<const Message> msg;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) {
                lock.unlock();
                clearQueue();
                return;
            }
            msg = std::move(queue_.front());
            queue_.pop_front();
        }
        auto logger = log::get();
        logger->trace("msg recv: {}->{},0x{:08x},{}", msg->sender, name_, msg->id, msg->payload.dump());
        try {
            handleMessage(msg->id, msg->payload);
        } catch (const std::exception& e) {
            logger->error("{}: message 0x{:08x} failed: {}", name_, msg->id, e.what());
        }
        logger->trace("msg end : {}->{},0x{:08x}", msg->sender, name_, msg->id);
    }
}

void MessageCore::clearQueue() {
    std::lock_guard lock(mutex_);
    queue_.clear();
}

// --- Service -----------------------------------------------------------------

bool Service::start(const std::shared_ptr<IServiceManager>& manager) {
    if (!manager) {
        log::get()->error("This service is not activator!");
        return false;
    }
    start();
    return true;
}

void Service::start() {
    startLoop();
    state_ = ServiceState::Started;
    log::get()->debug("started  :: {}", getName());
}

void Service::finish() {
    if (!loopRunning())
        return;
    stopLoop();
    state_ = ServiceState::Stopped;
    log::get()->debug("finished :: {}", getName());
}

// --- ServiceManager ----------------------------------------------------------

class ServiceManager::Sender : public IMessageSender {
public:
    void sendMessage(const std::string& dest, std::unique_ptr<const Message> msg) override {
        std::shared_ptr<Service> target;
        {
            std::lock_guard lock(mutex);
            auto it = byName.find(dest);
            if (it != byName.end())
                target = it->second;
        }
        if (!target) {
            log::get()->critical("[ASSERT] sendMessage: no service '{}'", dest);
            return;
        }
        target->postMessage(std::move(msg));
    }

    std::mutex mutex;
    std::map<std::string, std::shared_ptr<Service>> byName;
    std::vector<std::shared_ptr<Service>> registration;
};

ServiceManager::ServiceManager() : sender_(std::make_shared<Sender>()) {}

void ServiceManager::registerService(const std::shared_ptr<Service>& service) {
    {
        std::lock_guard lock(sender_->mutex);
        if (!sender_->byName.emplace(service->getName(), service).second)
            return;
        sender_->registration.push_back(service);
    }
    service->setMessageSender(sender_);
    log::get()->info("service registered: {}", service->getName());
}

std::vector<std::shared_ptr<Service>> ServiceManager::inStartOrder() const {
    std::lock_guard lock(sender_->mutex);
    return {sender_->registration.rbegin(), sender_->registration.rend()};
}

void ServiceManager::start(const std::string& name) {
    std::shared_ptr<Service> service;
    {
        std::lock_guard lock(sender_->mutex);
        auto it = sender_->byName.find(name);
        if (it != sender_->byName.end())
            service = it->second;
    }
    if (!service) {
        log::get()->critical("[ASSERT] start: no service '{}'", name);
        return;
    }
    service->start(shared_from_this());
}

void ServiceManager::startAll() {
    for (const auto& service : inStartOrder()) {
        if (service->getServiceState() == ServiceState::Stopped) {
            service->start();
            log::get()->info("service started: {}", service->getName());
        }
    }
}

void ServiceManager::finishAll() {
    for (const auto& service : inStartOrder()) {
        if (service->getServiceState() != ServiceState::Stopped) {
            service->finish();
            log::get()->info("service finished: {}", service->getName());
        }
    }
}

void ServiceManager::finishAllOthers(const std::string& name) {
    for (const auto& service : inStartOrder()) {
        if (service->getName() != name && service->getServiceState() != ServiceState::Stopped) {
            service->finish();
            log::get()->info("service finished: {}", service->getName());
        }
    }
}

}  // namespace tcng::core
