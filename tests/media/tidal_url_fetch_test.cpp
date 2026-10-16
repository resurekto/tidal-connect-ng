#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "media/TidalUrlFetchTask.hpp"
#include "util/HttpClient.hpp"

using namespace tcng::media;

namespace {

std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += "=";
    }
    return out;
}

ServerInfo sampleServerInfo() {
    const auto j = nlohmann::json::parse(R"({
        "serverUrl": "https://api.example.com/v1",
        "authInfo": {
            "headerAuth": "Bearer APPTOKEN",
            "oauthServerInfo": {
                "serverUrl": "https://auth.example.com/oauth2/token",
                "authInfo": {"headerAuth": "x", "oauthParameters": {"accessToken": "ACC", "refreshToken": "REF"}},
                "httpHeaderFields": ["A: b"],
                "formParameters": {"Grant_Type": "switch_client", "scope": "r_usr"}
            }
        },
        "queryParameters": {"AudioQuality": "lossless", "countryCode": "us", "assetpresentation": "PREVIEW"},
        "httpHeaderFields": ["X-Foo: bar"]
    })");
    return ServerInfo::fromJson(j);
}

}  // namespace

TEST_CASE("ServerInfo::fromJson walks contentServerInfo and lower-cases map keys") {
    const ServerInfo si = sampleServerInfo();
    CHECK(si.serverUrl == "https://api.example.com/v1");
    CHECK(si.headerAuth == "Bearer APPTOKEN");
    CHECK(si.oauth.serverUrl == "https://auth.example.com/oauth2/token");
    CHECK(si.oauth.accessToken == "ACC");
    CHECK(si.oauth.refreshToken == "REF");
    CHECK(si.oauth.formParameters.at("grant_type") == "switch_client");
    CHECK(si.queryParameters.at("audioquality") == "lossless");
    CHECK(si.httpHeaderFields == std::vector<std::string>{"X-Foo: bar"});
}

TEST_CASE("stream URL request: path, header and query (defaults win, quality upper-cased)") {
    const ServerInfo si = sampleServerInfo();
    const tcng::util::HttpRequest req = TidalUrlFetchTask::buildStreamUrlRequest(si, "TOK", "12345", 0);
    CHECK(req.method == tcng::util::HttpMethod::Get);
    CHECK(req.url == "https://api.example.com/v1/tracks/12345/playbackinfopostpaywall");
    CHECK(req.headers == std::vector<std::string>{"authorization: Bearer TOK"});
    CHECK(tcng::util::HttpClient::buildUrl(req.url, req.query) ==
          "https://api.example.com/v1/tracks/12345/playbackinfopostpaywall"
          "?assetpresentation=FULL&audioquality=LOSSLESS&countrycode=us&playbackmode=STREAM");

    const tcng::util::HttpRequest video = TidalUrlFetchTask::buildStreamUrlRequest(si, "TOK", "9", 1);
    CHECK(video.url == "https://api.example.com/v1/videos/9/playbackinfopostpaywall");
    CHECK(video.query.at("videoquality") == "AUDIO_ONLY");
}

TEST_CASE("token switch request: POST form body sorted, defaults not overridden") {
    ServerInfo si = sampleServerInfo();
    si.oauth.formParameters["client_id"] = "ignored";
    const tcng::util::HttpRequest req = TidalUrlFetchTask::buildTokenSwitchUrlRequest(si, "APP", "CID");
    CHECK(req.method == tcng::util::HttpMethod::Post);
    CHECK(req.url == "https://auth.example.com/oauth2/token");
    CHECK(req.headers == std::vector<std::string>{"Content-Type: application/x-www-form-urlencoded"});
    CHECK(req.body == "access_token=APP&client_id=CID&grant_type=switch_client&scope=r_usr");
}

TEST_CASE("app token from headerAuth, accessToken fallback") {
    ServerInfo si;
    si.headerAuth = "Bearer abc";
    CHECK(TidalUrlFetchTask::appTokenFrom(si) == "abc");
    si.headerAuth = "abc";  // no "Bearer ": TCNG uses the whole value
    CHECK(TidalUrlFetchTask::appTokenFrom(si) == "abc");
    si.headerAuth.clear();
    si.oauth.accessToken = "acc";
    CHECK(TidalUrlFetchTask::appTokenFrom(si) == "acc");
    si.oauth.accessToken.clear();
    CHECK(TidalUrlFetchTask::appTokenFrom(si).empty());
}

TEST_CASE("token switch response parsing") {
    CHECK(TidalUrlFetchTask::parseTokenSwitchUrlRequest(R"({"access_token":"T1","expires_in":5})") == "T1");
    CHECK_FALSE(TidalUrlFetchTask::parseTokenSwitchUrlRequest(R"({"token":"T1"})"));
    CHECK_FALSE(TidalUrlFetchTask::parseTokenSwitchUrlRequest("not json"));
}

TEST_CASE("base64 decoding") {
    CHECK(TidalUrlFetchTask::decodeBase64(base64("hello world!")) == "hello world!");
    CHECK(TidalUrlFetchTask::decodeBase64(base64("ab")) == "ab");
    CHECK(TidalUrlFetchTask::decodeBase64(base64("a")) == "a");
    CHECK_FALSE(TidalUrlFetchTask::decodeBase64("ab$c"));
}

TEST_CASE("playbackinfo response with a BTS manifest -> AudioItem") {
    auto make = [](const std::string& codecs, const std::string& enc = "NONE") {
        const nlohmann::json manifest = {{"mimeType", "audio/flac"},
                                         {"codecs", codecs},
                                         {"encryptionType", enc},
                                         {"keyId", "KEY"},
                                         {"urls", {"https://cdn.example.com/a.flac?token=x"}}};
        const nlohmann::json body = {{"trackId", 1},
                                     {"audioMode", "STEREO"},
                                     {"audioQuality", "LOSSLESS"},
                                     {"manifestMimeType", "application/vnd.tidal.bts"},
                                     {"manifest", base64(manifest.dump())}};
        return body.dump();
    };
    AudioItem item;
    REQUIRE(TidalUrlFetchTask::parseStreamUrlRequest(make("flac"), item));
    CHECK(item.mediaformat == media_format::kFlac);
    CHECK(item.audioCodec == audio_codec::kFlac);
    CHECK(item.streamType == 0);
    CHECK(item.url == "https://cdn.example.com/a.flac?token=x");
    CHECK(item.keyId.empty());
    CHECK(item.customData == R"({"audioMode":"STEREO","audioQuality":"LOSSLESS"})");

    AudioItem aac;
    REQUIRE(TidalUrlFetchTask::parseStreamUrlRequest(make("mp4a.40.2", "OLD_AES"), aac));
    CHECK(aac.mediaformat == media_format::kAac);
    CHECK(aac.audioCodec == audio_codec::kAacLc);
    CHECK(aac.keyId == "KEY");

    AudioItem he;
    REQUIRE(TidalUrlFetchTask::parseStreamUrlRequest(make("mp4a.40.5"), he));
    CHECK(he.audioCodec == audio_codec::kHeAac);

    AudioItem mqa;
    REQUIRE(TidalUrlFetchTask::parseStreamUrlRequest(make("mqa"), mqa));
    CHECK(mqa.mediaformat == media_format::kUnknown);
    CHECK(mqa.audioCodec == audio_codec::kUnknown);

    AudioItem bad;
    CHECK_FALSE(TidalUrlFetchTask::parseStreamUrlRequest(R"({"manifest":"!!!"})", bad));
    CHECK_FALSE(TidalUrlFetchTask::parseStreamUrlRequest(R"({"nomanifest":1})", bad));
}

TEST_CASE("tcng::util::HttpClient helpers") {
    CHECK(tcng::util::HttpClient::buildUrl("https://x/y", {}) == "https://x/y");
    CHECK(tcng::util::HttpClient::buildUrl("https://x/y", {{"b", "2"}, {"a", "1"}}) == "https://x/y?a=1&b=2");
    CHECK(tcng::util::HttpClient::hostOf("https://api.tidal.com:443/v1/x") == "api.tidal.com:443");
    CHECK(tcng::util::HttpClient::hostOf("api.tidal.com") == "api.tidal.com");
    CHECK(tcng::util::HttpClient::statusFromCurlCode(0) == 200);
    CHECK(tcng::util::HttpClient::statusFromCurlCode(28) == 11);
    CHECK(tcng::util::HttpClient::statusFromCurlCode(60) == 99);
    CHECK(tcng::util::HttpClient::statusFromCurlCode(35) == 10);
    CHECK(tcng::util::HttpClient::statusFromCurlCode(42) == 20);
    CHECK(tcng::util::HttpClient::statusFromCurlCode(7) == 99);
}
