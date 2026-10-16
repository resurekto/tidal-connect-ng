#include "util/Log.hpp"

#include <mutex>

#include <spdlog/sinks/stdout_color_sinks.h>

namespace tcng::util::log {

namespace {

std::mutex gMutex;

spdlog::sink_ptr sink() {
    static auto s = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    return s;
}

}  // namespace

std::shared_ptr<spdlog::logger> get(const std::string& name) {
    if (auto existing = spdlog::get(name))
        return existing;
    std::lock_guard lock(gMutex);
    if (auto existing = spdlog::get(name))
        return existing;
    auto logger = std::make_shared<spdlog::logger>(name, sink());
    logger->set_level(spdlog::get_level());
    spdlog::register_logger(logger);
    return logger;
}

void setLevel(spdlog::level::level_enum level) {
    std::lock_guard lock(gMutex);
    spdlog::set_level(level);  // applies to all registered loggers
}

spdlog::level::level_enum levelFromCli(int level) {
    switch (level) {
    case 0: return spdlog::level::off;
    case 1: return spdlog::level::critical;
    case 2: return spdlog::level::err;
    case 3: return spdlog::level::warn;
    case 4: return spdlog::level::info;
    case 5: return spdlog::level::debug;
    case 6: return spdlog::level::trace;
    default: return spdlog::level::info;
    }
}

}  // namespace tcng::util::log
