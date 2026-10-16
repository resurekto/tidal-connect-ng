#include "media/PythonTidalFetchTask.hpp"

#include <filesystem>
#include <fstream>
#include <random>

#include <cstdlib>

#include <nlohmann/json.hpp>

#include "embedded/PythonTidalHelper.hpp"
#include "util/Log.hpp"
#include "media/TidalUrlFetchTask.hpp"

namespace tcng::media {

namespace {

// python-tidal first, then whatever PYTHONPATH the environment already has.
std::string pythonPathWith(const std::string& tidalPath) {
    std::string pythonPath = tidalPath;
    if (const char* existing = std::getenv("PYTHONPATH"); existing && *existing)
        pythonPath += util::Subprocess::pathListSeparator() + std::string(existing);
    return pythonPath;
}

}  // namespace

namespace {

using Json = nlohmann::json;

std::string stringMember(const Json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

template <typename T>
T numberMember(const Json& j, const char* key, T fallback = 0) {
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<T>() : fallback;
}

// The last non-empty line: python libraries may print to stdout before the helper does.
std::string lastLine(const std::string& out) {
    auto end = out.find_last_not_of(" \t\r\n");
    if (end == std::string::npos)
        return {};
    const auto begin = out.find_last_of('\n', end);
    return out.substr(begin == std::string::npos ? 0 : begin + 1, end + 1 - (begin == std::string::npos ? 0 : begin + 1));
}

std::string tail(const std::string& s, std::size_t n = 600) {
    const auto end = s.find_last_not_of(" \t\r\n");
    if (end == std::string::npos)
        return {};
    const std::string t = s.substr(0, end + 1);
    return t.size() > n ? "..." + t.substr(t.size() - n) : t;
}

void setBody(util::HttpResponse& response, const std::string& body) {
    response.body.assign(body.begin(), body.end());
}

}  // namespace

PythonTidalFetchTask::PythonTidalFetchTask(PythonTidalConfig config, TrackFetchRequest request)
    : config_(std::move(config)), request_(std::move(request)) {}

void PythonTidalFetchTask::getResult(AudioItem& item, std::string& switchedToken) const {
    item = result_;
    switchedToken = request_.switchedToken;  // not used on this path; keep the engine's cache
}

std::optional<std::string> PythonTidalFetchTask::resolvePython(const std::string& configured) {
    if (!configured.empty())
        return util::Subprocess::findExecutable(configured);
#ifdef _WIN32
    // python3.exe on Windows is often the Microsoft Store stub: try python.exe first.
    for (const char* name : {"python", "python3"})
#else
    for (const char* name : {"python3", "python"})
#endif
        if (auto found = util::Subprocess::findExecutable(name))
            return found;
    return std::nullopt;
}

void PythonTidalFetchTask::fail(const std::string& message) {
    response_ = util::HttpResponse{};
    response_.status = 99;
    setBody(response_, Json{{"error", message}}.dump());
    log::get()->error("python-tidal: track={}: {}", request_.mediaId, message);
}

bool PythonTidalFetchTask::parseHelperOutput(const std::string& out, AudioItem& item, util::HttpResponse& response) {
    response = util::HttpResponse{};
    const Json j = Json::parse(lastLine(out), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        response.status = 99;
        setBody(response, Json{{"error", "unreadable helper output"}}.dump());
        return false;
    }
    if (!j.value("ok", false)) {
        const int status = numberMember<int>(j, "httpStatus");
        const std::string body = stringMember(j, "body");
        response.status = status ? status : 99;
        // TIDAL's own error JSON (with subStatus) when there is one, so that
        // PlaybackEngineTidal::checkStatusCode can classify it like the built-in path.
        setBody(response, body.empty() ? Json{{"error", stringMember(j, "error")}}.dump() : body);
        return false;
    }

    AudioItem result;
    TidalUrlFetchTask::applyCodecs(stringMember(j, "codecs"), result);
    result.customData = Json{{"audioMode", stringMember(j, "audioMode")}, {"audioQuality", stringMember(j, "audioQuality")}}.dump();
    result.durationSeconds = numberMember<double>(j, "duration");
    if (auto it = j.find("segments"); it != j.end() && it->is_array() && !it->empty()) {
        for (const auto& s : *it) {
            if (!s.is_object() || stringMember(s, "url").empty()) {
                response.status = 99;
                setBody(response, Json{{"error", "segment without url"}}.dump());
                return false;
            }
            result.segments.push_back({stringMember(s, "url"), numberMember<double>(s, "duration")});
        }
        result.initUrl = stringMember(j, "initUrl");
        if (result.initUrl.empty()) {  // the FLAC header lives in the init segment
            response.status = 99;
            setBody(response, Json{{"error", "segmented stream without an init segment"}}.dump());
            return false;
        }
        result.url = result.initUrl;
    } else {
        result.url = stringMember(j, "url");
    }
    if (result.url.empty()) {
        response.status = 99;
        setBody(response, Json{{"error", "no stream url"}}.dump());
        return false;
    }
    item = std::move(result);
    response.status = 200;
    return true;
}

void PythonTidalFetchTask::execute() {
    if (request_.type == 1) {
        fail("videos are not supported with --python-tidal-path");
        return;
    }
    util::Subprocess::Options options;
    options.program = config_.pythonBin;
    options.args = {"-", "resolve", request_.mediaId, "--session", config_.sessionPath};
    options.env = {{"PYTHONPATH", pythonPathWith(config_.tidalPath)}, {"PYTHONIOENCODING", "utf-8"}};
    options.input = std::string(embedded::pythonTidalHelperScript());
    options.timeout = config_.timeout;

    log::get()->info("python-tidal request: media={}", request_.mediaId);
    const auto run = process_.run(options);
    if (!run.started) {
        fail(run.error);
        return;
    }
    if (run.cancelled) {
        response_ = util::HttpResponse{};
        response_.status = 20;  // as HttpClient's cancelled pseudo status
        return;
    }
    if (run.timedOut) {
        fail("helper timed out after " + std::to_string(config_.timeout.count()) + " ms; stderr: " + tail(run.err));
        return;
    }
    AudioItem item;
    if (!parseHelperOutput(run.out, item, response_)) {
        const Json j = Json::parse(lastLine(run.out), nullptr, false);
        const std::string error = j.is_object() ? stringMember(j, "error") : std::string();
        log::get()->error("python-tidal: track={} failed (exit {}, status {}): {}{}", request_.mediaId, run.exitCode,
                        response_.status, error.empty() ? "unreadable output" : error,
                        run.err.empty() ? "" : "; stderr: " + tail(run.err));
        return;
    }
    const Json j = Json::parse(lastLine(run.out), nullptr, false);
    log::get()->info("python-tidal: track={} audioQuality={} manifestMimeType={} codecs={} bitDepth={} sampleRate={} segments={}",
                   stringMember(j, "trackId"), stringMember(j, "audioQuality"), stringMember(j, "manifestMimeType"),
                   stringMember(j, "codecs"), numberMember<int>(j, "bitDepth"), numberMember<int>(j, "sampleRate"),
                   item.segments.size());
    result_ = std::move(item);
}

int runPythonTidalLogin(const PythonTidalConfig& config) {
    namespace fs = std::filesystem;
    std::error_code ec;
    // The embedded script, written out for the duration of the login (python needs a file
    // when stdin is the terminal).
    std::random_device random;
    const fs::path script = fs::temp_directory_path(ec) / ("tcng-python-tidal-helper-" + std::to_string(random()) + ".py");
    {
        std::ofstream out(script, std::ios::binary);
        const auto source = embedded::pythonTidalHelperScript();
        out.write(source.data(), static_cast<std::streamsize>(source.size()));
        if (!out) {
            log::get()->error("python-tidal login: cannot write {}", script.string());
            return 1;
        }
    }
    std::string error;
    const int code = util::Subprocess::runInteractive(config.pythonBin, {script.string(), "login", "--session", config.sessionPath},
                                                {{"PYTHONPATH", pythonPathWith(config.tidalPath)}, {"PYTHONIOENCODING", "utf-8"}},
                                                error);
    fs::remove(script, ec);
    if (code < 0) {
        log::get()->error("python-tidal login: {}", error);
        return 1;
    }
    return code;
}

}  // namespace tcng::media
