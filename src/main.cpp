// TCNG: TIDAL Connect receiver.

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <thread>

#include "CommandLine.hpp"
#include "TidalConnect.hpp"
#include "util/Log.hpp"

#ifndef TCNG_VERSION
#define TCNG_VERSION "0.0.0"
#endif

namespace {

std::atomic<bool> gStop{false};

void onSignal(int) {
    gStop = true;
}

void installSignalHandlers() {
    // SIGINT and SIGTERM (systemd, docker stop) both stop in order.
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, onSignal);  // Windows console close / Ctrl+Break
#endif
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);    // a vanished peer must not kill the process
#endif
}

}  // namespace

int main(int argc, char** argv) {
    installSignalHandlers();
    // Create the shared "tcng" logger before any component logs (the media library
    // looks it up by name).
    spdlog::set_default_logger(tcng::log::get());
    try {
        tcng::Config config;
        switch (tcng::parseCommandLine(argc, argv, config, std::cout, std::cerr)) {
        case tcng::ParseResult::HelpShown: return 0;
        case tcng::ParseResult::Error: return 1;
        case tcng::ParseResult::Run: break;
        }
        tcng::log::setLevel(config.logLevel ? tcng::log::levelFromCli(*config.logLevel) : spdlog::level::info);
        if (config.pythonTidalLogin)
            return tcng::runPythonTidalLogin(config);

        tcng::TidalConnect app(config);
        std::cout << "STARTING TidalConnect services, version: " << TCNG_VERSION << std::endl;
        app.start();
        std::cout << "STARTED TidalConnect services." << std::endl;
        while (!gStop)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "STOPPING TidalConnect services." << std::endl;
        app.stop();
        std::cout << "STOPPED TidalConnect services." << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
