#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "FakePlayer.hpp"
#include "media/PlaybackEngineTidal.hpp"
#include "media/PythonTidalFetchTask.hpp"
#include "util/Subprocess.hpp"

using namespace tcng::media;
using namespace tcng::test;
using RA = PlaybackEngineGeneric::RecoveryAction;
namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A python-tidal stand-in: just the parts of tidalapi the helper uses.
//   track 1: DASH (FLAC, 3 segments), 2: BTS, 404: not found, 7: AAC DASH.
const char* kFakeTidalapi = R"PY(
import base64, json

MPD = """<?xml version='1.0'?>
<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" mediaPresentationDuration="PT8.5S" type="static">
 <Period><AdaptationSet contentType="audio" mimeType="audio/mp4">
  <Representation id="FLAC,96000,24" codecs="CODEC" bandwidth="3000000" audioSamplingRate="96000">
   <SegmentTemplate timescale="96000" initialization="https://cdn/t/0.mp4?tok=a&amp;x=1"
                    media="https://cdn/t/$Number$.mp4?tok=a&amp;x=1" startNumber="1">
    <SegmentTimeline><S d="384000" r="1"/><S d="48000"/></SegmentTimeline>
   </SegmentTemplate>
  </Representation>
 </AdaptationSet></Period>
</MPD>"""

class ObjectNotFound(Exception):
    pass

class Stream:
    def __init__(self, mime, manifest):
        self.manifest_mime_type = mime
        self.manifest = base64.b64encode(manifest.encode()).decode()
        self.audio_quality = "HI_RES_LOSSLESS" if "dash" in mime else "LOSSLESS"
        self.audio_mode = "STEREO"
        self.bit_depth = 24 if "dash" in mime else 16
        self.sample_rate = 96000 if "dash" in mime else 44100

class Track:
    def __init__(self, session, tid):
        self.session, self.id, self.duration = session, tid, 9
    def get_stream(self):
        if self.session.audio_quality != "HI_RES_LOSSLESS":
            raise RuntimeError("quality not requested")
        if self.id == "2":
            bts = {"mimeType": "audio/flac", "codecs": "flac", "encryptionType": "NONE", "urls": ["https://cdn/f.flac"]}
            return Stream("application/vnd.tidal.bts", json.dumps(bts))
        return Stream("application/dash+xml", MPD.replace("CODEC", "mp4a.40.2" if self.id == "7" else "flac"))

class Session:
    def __init__(self):
        self.access_token = "A"
        self.audio_quality = None
    def load_session_from_file(self, path):
        return json.loads(path.read_text()).get("valid", False)
    def save_session_to_file(self, path):
        pass
    def track(self, tid):
        if tid == "404":
            raise ObjectNotFound("Track not found")
        return Track(self, tid)
)PY";

struct FakePythonTidal {
    fs::path root;
    PythonTidalConfig config;
    bool available = false;

    FakePythonTidal() {
        root = fs::temp_directory_path() / ("tcng-pytidal-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        writeFile(root / "checkout" / "tidalapi" / "__init__.py", kFakeTidalapi);
        writeFile(root / "session.json", R"({"valid": true})");
        writeFile(root / "bad-session.json", R"({"valid": false})");
        const auto python = PythonTidalFetchTask::resolvePython("");
        available = python.has_value();
        config.pythonBin = python.value_or("");
        config.tidalPath = (root / "checkout").string();
        config.sessionPath = (root / "session.json").string();
    }
    ~FakePythonTidal() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    std::pair<AudioItem, tcng::util::HttpResponse> resolve(const std::string& mediaId, unsigned type = 0,
                                               const std::string& session = "") {
        auto c = config;
        if (!session.empty())
            c.sessionPath = (root / session).string();
        PythonTidalFetchTask task(c, TrackFetchRequest{{}, "CID", mediaId, "TOKEN", type});
        task.execute();
        AudioItem item;
        std::string token;
        task.getResult(item, token);
        CHECK(token == "TOKEN");
        return {item, task.response()};
    }
};

}  // namespace

TEST_CASE("tcng::util::Subprocess: Windows argument quoting") {
    CHECK(tcng::util::Subprocess::quoteWindowsArg("plain") == "plain");
    CHECK(tcng::util::Subprocess::quoteWindowsArg("") == "\"\"");
    CHECK(tcng::util::Subprocess::quoteWindowsArg("a b") == "\"a b\"");
    CHECK(tcng::util::Subprocess::quoteWindowsArg(R"(say "hi")") == R"("say \"hi\"")");
    CHECK(tcng::util::Subprocess::quoteWindowsArg(R"(C:\dir with space\)") == R"("C:\dir with space\\")");
    CHECK(tcng::util::Subprocess::quoteWindowsArg(R"(C:\no\space)") == R"(C:\no\space)");
    CHECK(tcng::util::Subprocess::quoteWindowsArg(R"(a\"b)") == R"("a\\\"b")");
}

TEST_CASE("tcng::util::Subprocess: missing executables are reported, not started") {
    tcng::util::Subprocess p;
    const auto r = p.run({.program = "tcng-no-such-program-xyz"});
    CHECK_FALSE(r.started);
    CHECK(r.error.find("not found") != std::string::npos);
    CHECK_FALSE(tcng::util::Subprocess::findExecutable("tcng-no-such-program-xyz"));
}

#ifndef _WIN32
TEST_CASE("tcng::util::Subprocess: stdin, stdout, stderr, exit code and environment") {
    tcng::util::Subprocess p;
    const std::string big(300 * 1024, 'x');  // larger than a pipe buffer both ways
    const auto r = p.run({.program = "sh",
                          .args = {"-c", "cat; echo \"$TCNG_TEST_VAR\" >&2; exit 3"},
                          .env = {{"TCNG_TEST_VAR", "hello"}},
                          .input = big});
    REQUIRE(r.started);
    CHECK(r.exitCode == 3);
    CHECK(r.out == big);
    CHECK(r.err == "hello\n");
    CHECK_FALSE(r.timedOut);
}

TEST_CASE("tcng::util::Subprocess: a child that does not read stdin does not hurt the parent") {
    tcng::util::Subprocess p;
    const auto r = p.run({.program = "sh", .args = {"-c", "exit 0"}, .input = std::string(1 << 20, 'y')});
    REQUIRE(r.started);
    CHECK(r.exitCode == 0);
}

TEST_CASE("tcng::util::Subprocess: timeout and cancel kill the child") {
    {
        tcng::util::Subprocess p;
        const auto start = std::chrono::steady_clock::now();
        const auto r = p.run({.program = "sh", .args = {"-c", "sleep 10"}, .timeout = std::chrono::milliseconds(200)});
        CHECK(r.timedOut);
        CHECK(r.exitCode == -1);
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    }
    {
        tcng::util::Subprocess p;
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            p.cancel();
        });
        const auto r = p.run({.program = "sh", .args = {"-c", "sleep 10"}});
        canceller.join();
        CHECK(r.cancelled);
        CHECK_FALSE(r.timedOut);
    }
}
#endif

TEST_CASE("python-tidal: helper output -> AudioItem / response") {
    AudioItem item;
    tcng::util::HttpResponse resp;

    SECTION("DASH") {
        const std::string out = "noise from a library\n" + nlohmann::json{
            {"ok", true}, {"trackId", "1"}, {"audioQuality", "HI_RES_LOSSLESS"}, {"audioMode", "STEREO"},
            {"codecs", "flac"}, {"duration", 8.5}, {"initUrl", "https://cdn/0.mp4"},
            {"segments", {{{"url", "https://cdn/1.mp4"}, {"duration", 4.0}}, {{"url", "https://cdn/2.mp4"}, {"duration", 4.5}}}}}.dump() + "\n";
        REQUIRE(PythonTidalFetchTask::parseHelperOutput(out, item, resp));
        CHECK(resp.status == 200);
        CHECK(item.mediaformat == media_format::kFlac);
        CHECK(item.audioCodec == audio_codec::kFlac);
        CHECK(item.initUrl == "https://cdn/0.mp4");
        CHECK(item.url == "https://cdn/0.mp4");
        REQUIRE(item.segments.size() == 2);
        CHECK(item.segments[1].url == "https://cdn/2.mp4");
        CHECK(item.segments[1].durationSeconds == 4.5);
        CHECK(item.durationSeconds == 8.5);
        CHECK(nlohmann::json::parse(item.customData) == nlohmann::json{{"audioMode", "STEREO"}, {"audioQuality", "HI_RES_LOSSLESS"}});
    }
    SECTION("single file") {
        REQUIRE(PythonTidalFetchTask::parseHelperOutput(
            R"({"ok": true, "codecs": "flac", "url": "https://cdn/f.flac", "audioQuality": "LOSSLESS"})", item, resp));
        CHECK(item.url == "https://cdn/f.flac");
        CHECK(item.segments.empty());
        CHECK(item.initUrl.empty());
    }
    SECTION("TIDAL error body is passed through for checkStatusCode") {
        CHECK_FALSE(PythonTidalFetchTask::parseHelperOutput(
            R"({"ok": false, "error": "x", "httpStatus": 404, "body": "{\"subStatus\": 4005}"})", item, resp));
        CHECK(resp.status == 404);
        CHECK(PlaybackEngineTidal::classifyStatus(resp, nullptr) == RA::SkipTrack);
    }
    SECTION("errors without HTTP status are 99") {
        CHECK_FALSE(PythonTidalFetchTask::parseHelperOutput(R"({"ok": false, "error": "no session"})", item, resp));
        CHECK(resp.status == 99);
        CHECK(resp.bodyString().find("no session") != std::string::npos);
        CHECK_FALSE(PythonTidalFetchTask::parseHelperOutput("Traceback (most recent call last):", item, resp));
        CHECK(resp.status == 99);
        CHECK_FALSE(PythonTidalFetchTask::parseHelperOutput(R"({"ok": true, "codecs": "flac"})", item, resp));
        CHECK_FALSE(PythonTidalFetchTask::parseHelperOutput(
            R"({"ok": true, "codecs": "flac", "segments": [{"url": "s1", "duration": 1}]})", item, resp));  // no init
    }
}

TEST_CASE("python-tidal: the embedded helper against a fake tidalapi") {
    FakePythonTidal fake;
    if (!fake.available)
        SKIP("no python3/python on PATH");

    SECTION("DASH: init + segments from the MPD timeline") {
        auto [item, resp] = fake.resolve("1");
        REQUIRE(resp.status == 200);
        CHECK(item.initUrl == "https://cdn/t/0.mp4?tok=a&x=1");
        REQUIRE(item.segments.size() == 3);
        CHECK(item.segments[0].url == "https://cdn/t/1.mp4?tok=a&x=1");
        CHECK(item.segments[2].url == "https://cdn/t/3.mp4?tok=a&x=1");
        CHECK(item.segments[0].durationSeconds == 4.0);
        CHECK(item.segments[2].durationSeconds == 0.5);
        CHECK(item.durationSeconds == 8.5);
        CHECK(item.audioCodec == audio_codec::kFlac);
    }
    SECTION("BTS: a single file") {
        auto [item, resp] = fake.resolve("2");
        REQUIRE(resp.status == 200);
        CHECK(item.url == "https://cdn/f.flac");
        CHECK(item.segments.empty());
        CHECK(item.durationSeconds == 9.0);
    }
    SECTION("not found -> skip track, like sub-status 4005") {
        auto [item, resp] = fake.resolve("404");
        CHECK(resp.status == 404);
        CHECK(PlaybackEngineTidal::classifyStatus(resp, nullptr) == RA::SkipTrack);
    }
    SECTION("failures fail the track") {
        CHECK(fake.resolve("7").second.status == 99);                       // AAC in DASH
        CHECK(fake.resolve("1", 0, "bad-session.json").second.status == 99);  // session rejected
        CHECK(fake.resolve("1", 0, "missing.json").second.status == 99);
        CHECK(fake.resolve("1", 1).second.status == 99);  // videos
        auto c = fake.config;
        c.tidalPath = (fake.root / "nowhere").string();  // no tidalapi to import
        PythonTidalFetchTask task(c, TrackFetchRequest{{}, "CID", "1", "", 0});
        task.execute();
        CHECK(task.response().status == 99);
        CHECK(task.response().bodyString().find("tidalapi") != std::string::npos);
    }
}

TEST_CASE("python-tidal: PlaybackEngineTidal prepares the resolved item") {
    FakePythonTidal fake;
    if (!fake.available)
        SKIP("no python3/python on PATH");
    auto player = std::make_shared<FakePlayer>();
    auto delegate = std::make_shared<RecordingDelegate>();
    auto engine = std::make_shared<PlaybackEngineTidal>("CID", std::make_shared<PythonTidalFetchTaskFactory>(fake.config));
    engine->connect(player, delegate);
    engine->prepare(5, ServerInfo{}, "1", 0);  // no app token needed on this path
    auto has = [](const std::vector<std::string>& ev, const std::string& prefix) {
        return std::any_of(ev.begin(), ev.end(), [&](const std::string& e) { return e.rfind(prefix, 0) == 0; });
    };
    REQUIRE(delegate->waitFor([&](const auto& ev) { return has(ev, "prepared:5:") || has(ev, "prepareError:5"); },
                              std::chrono::seconds(20)));
    CHECK(delegate->contains("prepared:5:https://cdn/t/0.mp4?tok=a&x=1"));
    {
        std::lock_guard l(player->m);
        REQUIRE(player->prepared.size() == 1);
        CHECK(player->prepared[0].segments.size() == 3);
    }
    engine->prepare(6, ServerInfo{}, "404", 0);
    REQUIRE(delegate->waitFor([&](const auto& ev) { return has(ev, "prepareError:6"); }, std::chrono::seconds(20)));
    CHECK(delegate->lastAction == RA::SkipTrack);
    CHECK(delegate->lastResponse.status == 404);
    engine->disconnect();
}
