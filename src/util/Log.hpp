#pragma once
// Logging. Everything goes through one spdlog logger named "tcng"
// with a coloured stdout sink.

#include <memory>
#include <string>

#include <spdlog/spdlog.h>

namespace tcng::util::log {

// Returns the named logger, creating it on first use. All loggers share one sink.
std::shared_ptr<spdlog::logger> get(const std::string& name = "tcng");

// Sets the level of every logger, present and future.
void setLevel(spdlog::level::level_enum level);

// Maps --log-level 0..6 (OFF, CRITICAL, ERROR, WARNING, INFO, DEBUG, TRACE) to spdlog.
spdlog::level::level_enum levelFromCli(int level);

}  // namespace tcng::util::log

namespace tcng {
namespace log = util::log;
}
