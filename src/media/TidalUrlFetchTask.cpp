#include "media/TidalUrlFetchTask.hpp"

#include <algorithm>
#include <array>
#include <cctype>

#include <nlohmann/json.hpp>

#include "util/Log.hpp"

namespace tcng::media {

namespace {

using Json = nlohmann::json;

std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string stringMember(const Json& j, const char* key) {
    if (!j.is_object())
        return {};
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

TidalUrlFetchTask::TidalUrlFetchTask(Request request)
    : request_(std::move(request)), client_(std::make_shared<util::HttpClient>(true)) {}

void TidalUrlFetchTask::enableServerVerification(bool enable) {
    verify_ = enable;
    client_->enableServerVerification(enable);
}

void TidalUrlFetchTask::cancel() {
    cancelled_ = true;
    client_->cancel();
}

void TidalUrlFetchTask::getResult(AudioItem& item, std::string& switchedToken) const {
    item = result_;
    switchedToken = request_.switchedToken;
}

std::string TidalUrlFetchTask::appTokenFrom(const ServerInfo& si) {
    if (!si.headerAuth.empty()) {
        const auto index = si.headerAuth.find(BEARER);
        if (index == std::string::npos) {
            // No "Bearer " prefix: use the whole value as the token.
            log::get()->error("[ASSERT] {}", "index != std::string::npos");
            return si.headerAuth;
        }
        return si.headerAuth.substr(index + 7);
    }
    return si.oauth.accessToken;
}

util::HttpRequest TidalUrlFetchTask::buildTokenSwitchUrlRequest(const ServerInfo& si, const std::string& appToken,
                                                          const std::string& clientId) {
    util::HttpRequest req;
    req.method = util::HttpMethod::Post;
    req.url = si.oauth.serverUrl;
    req.headers.push_back("Content-Type: application/x-www-form-urlencoded");
    std::map<std::string, std::string> form;
    form.emplace("access_token", appToken);
    form.emplace("client_id", clientId);
    for (const auto& [k, v] : si.oauth.formParameters)
        form.emplace(k, v);  // existing keys are not overwritten
    bool first = true;
    for (const auto& [k, v] : form) {
        if (!first)
            req.body += "&";
        first = false;
        req.body += util::HttpClient::urlEncode(k) + "=" + util::HttpClient::urlEncode(v);
    }
    return req;
}

util::HttpRequest TidalUrlFetchTask::buildStreamUrlRequest(const ServerInfo& si, const std::string& token,
                                                     const std::string& mediaId, unsigned type) {
    util::HttpRequest req;
    req.method = util::HttpMethod::Get;
    req.url = si.serverUrl + "/" + (type == 1 ? "videos" : "tracks") + "/" + mediaId + "/playbackinfopostpaywall";
    req.headers.push_back(std::string("authorization: ") + BEARER + token);
    req.query.emplace("assetpresentation", "FULL");
    req.query.emplace("playbackmode", "STREAM");
    if (type == 1)
        req.query.emplace("videoquality", "AUDIO_ONLY");
    for (const auto& [k, v] : si.queryParameters) {
        // Keys are already lower-cased (ServerInfo::fromJson); defaults cannot be overridden.
        if (k == "audioquality" || k == "audiomode")
            req.query.emplace(k, toUpper(v));
        else
            req.query.emplace(k, v);
    }
    return req;
}

std::optional<std::string> TidalUrlFetchTask::parseTokenSwitchUrlRequest(const std::string& body) {
    const Json j = Json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains(ACCESS_TOKEN) || !j[ACCESS_TOKEN].is_string()) {
        // The body may carry tokens: log its size only.
        log::get()->error("Invalid token response ({} bytes, {})", body.size(), j.is_discarded() ? "parse error" : "no access_token");
        return std::nullopt;
    }
    return j[ACCESS_TOKEN].get<std::string>();
}

// decodeBase64, standard alphabet; '=' padding ends the data.
std::optional<std::string> TidalUrlFetchTask::decodeBase64(const std::string& in) {
    static constexpr std::string_view kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(in.size() * 3 / 4);
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=')
            break;
        if (c == '\r' || c == '\n')
            continue;
        const auto pos = kTable.find(c);
        if (pos == std::string_view::npos) {
            log::get()->error("invalid character. c={}", c);
            return std::nullopt;
        }
        acc = (acc << 6) | static_cast<std::uint32_t>(pos);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xff));
        }
    }
    return out;
}

// codecs -> (mediaformat, audioCodec), from parseStreamUrlRequest. TCNG: "mqa" and "mha1"
// (MPEG-H) are unsupported and reported as unknown; the decoder is then chosen by probing.
void TidalUrlFetchTask::applyCodecs(const std::string& codecs, AudioItem& item) {
    if (codecs == "flac") {
        item.mediaformat = media_format::kFlac;
        item.audioCodec = audio_codec::kFlac;
    } else if (codecs == "mp4a.40.5") {
        item.mediaformat = media_format::kAac;
        item.audioCodec = audio_codec::kHeAac;
    } else if (codecs == "mp4a.40.2") {
        item.mediaformat = media_format::kAac;
        item.audioCodec = audio_codec::kAacLc;
    } else {
        item.mediaformat = media_format::kUnknown;
        item.audioCodec = audio_codec::kUnknown;
    }
}

bool TidalUrlFetchTask::parseStreamUrlRequest(const std::string& body, AudioItem& item) {
    const Json j = Json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("manifest") || !j["manifest"].is_string()) {
        log::get()->error("Invalid response: body={}, parse_error={}", body, j.is_discarded() ? "parse error" : "");
        return false;
    }
    const Json customData = {{"audioMode", stringMember(j, "audioMode")},
                             {"audioQuality", stringMember(j, "audioQuality")}};
    item.customData = customData.dump();
    log::get()->debug("custom_data={}", item.customData);
    auto field = [&](const char* key) -> std::string {
        if (!j.contains(key))
            return "-";
        return j[key].is_string() ? j[key].get<std::string>() : j[key].dump();
    };
    log::get()->info("playbackinfo: track={} audioQuality={} audioMode={} manifestMimeType={} bitDepth={} sampleRate={}",
                   field("trackId"), field("audioQuality"), field("audioMode"), field("manifestMimeType"),
                   field("bitDepth"), field("sampleRate"));

    const auto decoded = decodeBase64(j["manifest"].get<std::string>());
    const Json manifest = decoded ? Json::parse(*decoded, nullptr, false) : Json(nullptr);
    if (!decoded || manifest.is_discarded() || !manifest.is_object()) {
        log::get()->error("Invalid manifest: decoded={}, parse_error={}", decoded.value_or(""), "parse error");
        return false;
    }

    applyCodecs(stringMember(manifest, "codecs"), item);
    item.streamType = 0;
    if (stringMember(manifest, "encryptionType") == "OLD_AES") {
        item.keyId = stringMember(manifest, "keyId");
        log::get()->warn("encrypted stream (OLD_AES) is not supported; it will not decode");
    }
    if (manifest.contains("urls") && manifest["urls"].is_array() && !manifest["urls"].empty() &&
        manifest["urls"][0].is_string())
        item.url = manifest["urls"][0].get<std::string>();
    return true;
}

bool TidalUrlFetchTask::executeSwitchToken() {
    if (cancelled_)
        return false;
    const util::HttpRequest req = buildTokenSwitchUrlRequest(request_.serverInfo, appToken_, request_.clientId);
    log::get()->debug("token switch request to {}", req.url);  // the body holds the app token: never logged
    response_ = client_->request(req);
    if (response_.status != 200)
        return false;
    auto token = parseTokenSwitchUrlRequest(response_.bodyString());
    if (!token) {
        log::get()->error("Switch token error.");
        request_.switchedToken.clear();
        response_.status = 99;  // TCNG: see executeGetStreamUrl
        return false;
    }
    request_.switchedToken = *token;
    return true;
}

bool TidalUrlFetchTask::executeGetStreamUrl() {
    if (cancelled_)
        return false;
    const util::HttpRequest req =
        buildStreamUrlRequest(request_.serverInfo, request_.switchedToken, request_.mediaId, request_.type);
    auto quality = req.query.find("audioquality");
    log::get()->info("playbackinfo request: media={} audioquality={}", request_.mediaId,
                   quality == req.query.end() ? "(none)" : quality->second);
    response_ = client_->request(req);
    if (response_.status != 200) {
        log::get()->warn("playbackinfo: HTTP {} {}", response_.status, response_.bodyString().substr(0, 300));
        return false;
    }
    AudioItem item;
    if (!parseStreamUrlRequest(response_.bodyString(), item)) {
        log::get()->error("Get stream url error.");
        // A 200 with an unusable body must not be reported as success to the engine.
        response_.status = 99;
        return false;
    }
    result_ = item;
    return true;
}

void TidalUrlFetchTask::execute() {
    if (request_.switchedToken.empty()) {
        appToken_ = appTokenFrom(request_.serverInfo);
        if (appToken_.empty()) {
            log::get()->error("access_token is not provided.");
            return;
        }
        if (!executeSwitchToken())
            return;
    }
    if (executeGetStreamUrl())
        log::get()->debug("Http fetch succeeded.");
}

}  // namespace tcng::media
