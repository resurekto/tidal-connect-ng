#pragma once
// TCNG (--python-tidal-path): resolves a TIDAL track through python-tidal instead of the token
// switch + playbackinfopostpaywall of TidalUrlFetchTask, which TIDAL caps at LOSSLESS
// (16/44.1) for this receiver. Runs the embedded python-tidal-helper.py once per track:
//
//   <python> - resolve <mediaId> --session <file>     (script on stdin,
//                                                      PYTHONPATH=<python-tidal checkout>)
//
// The helper asks for HI_RES_LOSSLESS and prints one JSON object: a single file URL (BTS
// manifest) or an init URL plus media segments (DASH, FLAC in fragmented MP4). Any failure
// fails the track; there is no fallback to TidalUrlFetchTask.

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "media/ITrackFetchTask.hpp"
#include "util/Subprocess.hpp"

namespace tcng::media {

struct PythonTidalConfig {
    std::string pythonBin;    // interpreter (resolved, see resolvePython)
    std::string tidalPath;    // python-tidal checkout, prepended to PYTHONPATH
    std::string sessionPath;  // tidalapi session file
    std::chrono::milliseconds timeout{30000};
};

class PythonTidalFetchTask : public ITrackFetchTask {
public:
    PythonTidalFetchTask(PythonTidalConfig config, TrackFetchRequest request);

    void execute() override;
    void cancel() override { process_.cancel(); }
    void getResult(AudioItem& item, std::string& switchedToken) const override;
    const util::HttpResponse& response() const override { return response_; }

    // The helper's stdout -> item + response (200, or the helper's httpStatus/body, or 99).
    // Returns true on success. Exposed for tests.
    static bool parseHelperOutput(const std::string& out, AudioItem& item, util::HttpResponse& response);
    // --python-bin if given (a path or a name on PATH), else python3 / python from PATH.
    static std::optional<std::string> resolvePython(const std::string& configured);

private:
    void fail(const std::string& message);

    const PythonTidalConfig config_;
    TrackFetchRequest request_;
    util::Subprocess process_;
    util::HttpResponse response_;
    AudioItem result_;
};

// --python-tidal-login: runs the embedded helper's `login` interactively (it prints a TIDAL
// link and reads the redirect URL from the terminal) and writes config.sessionPath. Returns
// the process exit code to use.
int runPythonTidalLogin(const PythonTidalConfig& config);

class PythonTidalFetchTaskFactory : public ITrackFetchTaskFactory {
public:
    explicit PythonTidalFetchTaskFactory(PythonTidalConfig config) : config_(std::move(config)) {}
    std::shared_ptr<ITrackFetchTask> create(TrackFetchRequest request) const override {
        return std::make_shared<PythonTidalFetchTask>(config_, std::move(request));
    }

private:
    const PythonTidalConfig config_;
};

}  // namespace tcng::media
