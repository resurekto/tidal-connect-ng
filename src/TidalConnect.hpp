#pragma once
// Application wiring: builds the services from the
// command-line config, starts them in order and stops them.

#include <memory>

#include "Config.hpp"

namespace tcng::core {
class ServiceManager;
}
namespace tcng::net {
class Loop;
}
namespace tcng::media {
class ITrackFetchTaskFactory;
}

namespace tcng {

class TidalConnect {
public:
    explicit TidalConnect(Config config);
    ~TidalConnect();

    // Throws std::exception on configuration errors (e.g. empty client id).
    void start();
    void stop();

    // The TIDAL engine's track resolver: the built-in fetch, or python-tidal when
    // config.pythonTidalPath is set (validated here; throws on a bad path or missing python).
    static std::shared_ptr<const media::ITrackFetchTaskFactory> makeFetchTaskFactory(const Config& config);

private:
    Config config_;
    std::unique_ptr<net::Loop> loop_;
    std::shared_ptr<core::ServiceManager> manager_;
    bool started_ = false;
};

// --python-tidal-login: interactive login through python-tidal; returns the exit code.
int runPythonTidalLogin(const Config& config);

}  // namespace tcng
