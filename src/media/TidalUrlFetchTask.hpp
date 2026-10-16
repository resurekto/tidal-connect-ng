#pragma once
// TidalUrlFetchTask: resolves a TIDAL mediaId into a stream URL. Runs on
// PlaybackEngineTidal's 1-thread ThreadPool:
//   1. if no switched token is cached: POST the app token + client id to the OAuth
//      server's token-switch endpoint -> access_token;
//   2. GET {serverUrl}/{tracks|videos}/{mediaId}/playbackinfopostpaywall with
//      "authorization: Bearer <switched token>" -> base64 BTS manifest -> AudioItem.

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "media/IMediaPlayer.hpp"
#include "media/ITrackFetchTask.hpp"
#include "media/PlaybackEngineGeneric.hpp"
#include "util/HttpClient.hpp"

namespace tcng::media {

class TidalUrlFetchTask : public ITrackFetchTask {
public:
    using Request = TrackFetchRequest;

    static constexpr const char* BEARER = "Bearer ";
    static constexpr const char* ACCESS_TOKEN = "access_token";

    explicit TidalUrlFetchTask(Request request);

    void enableServerVerification(bool enable);
    void execute() override;
    // Marks the task cancelled and aborts the in-flight transfer.
    void cancel() override;

    // Result (valid after execute()): the AudioItem and the switched token to cache.
    void getResult(AudioItem& item, std::string& switchedToken) const override;
    const util::HttpResponse& response() const override { return response_; }

    // --- Pure helpers (exposed for tests) ---
    static util::HttpRequest buildTokenSwitchUrlRequest(const ServerInfo& si, const std::string& appToken, const std::string& clientId);
    static util::HttpRequest buildStreamUrlRequest(const ServerInfo& si, const std::string& token, const std::string& mediaId, unsigned type);
    static std::optional<std::string> parseTokenSwitchUrlRequest(const std::string& body);
    static bool parseStreamUrlRequest(const std::string& body, AudioItem& item);
    static std::optional<std::string> decodeBase64(const std::string& in);
    // Manifest "codecs" -> item.mediaformat / item.audioCodec.
    static void applyCodecs(const std::string& codecs, AudioItem& item);
    // headerAuth / accessToken -> app token, "" if none.
    static std::string appTokenFrom(const ServerInfo& si);

private:
    bool executeSwitchToken();
    bool executeGetStreamUrl();

    Request request_;
    std::string appToken_;
    std::shared_ptr<util::HttpClient> client_;
    bool verify_ = true;
    std::atomic<bool> cancelled_{false};
    util::HttpResponse response_;
    AudioItem result_;
};

class TidalUrlFetchTaskFactory : public ITrackFetchTaskFactory {
public:
    std::shared_ptr<ITrackFetchTask> create(TrackFetchRequest request) const override {
        return std::make_shared<TidalUrlFetchTask>(std::move(request));
    }
};

}  // namespace tcng::media
