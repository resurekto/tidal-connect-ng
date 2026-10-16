#pragma once
// An engine wraps the IMediaPlayer for one app. The PlaybackController picks it with
// lookupEngine(appId), connects it to the shared media player and drives it; the engine
// forwards player callbacks to the controller's Delegate, adding the current mediaSeqNo.
// PlaybackEngineGeneric serves non-TIDAL apps (plain srcUrl, or a URL fetched from
// ServerInfo with an AsyncHttpClient); PlaybackEngineTidal overrides the ServerInfo path.
//
// Threads: prepare/stop/reset/disconnect are called from the controller thread; the
// Delegate is called from the media player loop thread (status, progress, format,
// completion, errors, request-next) and from the engine's HTTP worker thread
// (onPrepared/onPreparedNext/onPrepare*Error). Delegate implementations must not call back
// into the engine synchronously from those callbacks (the controller self-posts instead).

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "media/IMediaPlayer.hpp"
#include "util/AsyncHttpClient.hpp"

namespace tcng::media {

// PlaybackEngineGeneric::ServerInfo: the app's contentServerInfo, parsed by
// the playback controller.
struct ServerInfo {
    std::string serverUrl;   // contentServerInfo.serverUrl: content API base URL
    std::string headerAuth;  // contentServerInfo.authInfo.headerAuth: "Bearer <app token>"
    struct OAuthServerInfo {
        std::string serverUrl;    // contentServerInfo.authInfo.oauthServerInfo.serverUrl: token-switch endpoint
        std::string headerAuth;   // ...oauthServerInfo.authInfo.headerAuth (unused)
        std::string accessToken;  // ...authInfo.oauthParameters.accessToken: fallback app token
        std::string refreshToken; // ...authInfo.oauthParameters.refreshToken (unused)
        std::vector<std::string> httpHeaderFields;            // (unused)
        std::map<std::string, std::string> formParameters;    // keys lower-cased
    } oauth;
    std::map<std::string, std::string> queryParameters;  // contentServerInfo.queryParameters, keys lower-cased
    std::vector<std::string> httpHeaderFields;           // contentServerInfo.httpHeaderFields

    // Mirrors procSetServerInfo: missing or non-string values become "" / skipped, object
    // keys are lower-cased. `contentServerInfo` is the J object above.
    static ServerInfo fromJson(const nlohmann::json& contentServerInfo);
};

class PlaybackEngineGeneric : public IMediaPlayer::Delegate,
                              public util::AsyncHttpClient::Delegate,
                              public std::enable_shared_from_this<PlaybackEngineGeneric> {
public:
    // What to do after a failed fetch, decided by PlaybackEngineTidal::checkStatusCode.
    enum class RecoveryAction : int {
        // Any other failure (incl. token errors 11001-11003, which also clear the cached
        // token). prepare: stop + notifyContentServerError + queue tokenExpired;
        // prepareNext: queue tokenExpired(isNextMedia=true).
        Default = 0,
        // HTTP 4xx with subStatus 4005/4007 (track not playable). prepare: skip to the next
        // queue item; prepareNext: ignored.
        SkipTrack = 1,
    };

    // Delegate, implemented by the PlaybackController. `seq` is the engine's current
    // mediaSeqNo.
    class Delegate {
    public:
        virtual ~Delegate() = default;
        virtual void onPrepared(std::uint32_t seq, const AudioItem& item) = 0;
        virtual void onStatusUpdated(const PlayStatus& status, const AudioItem& item) = 0;
        virtual void onProgressUpdated(std::uint64_t positionMs, std::uint64_t durationMs) = 0;
        virtual void onAudioFormatUpdated(const AudioFormat& format) = 0;
        virtual void onPlaybackCompleted(std::uint32_t seq, bool hasNext) = 0;
        virtual void onPrepareError(std::uint32_t seq, RecoveryAction action, const util::HttpResponse& response) = 0;
        virtual void onPrepareNextError(std::uint32_t seq, RecoveryAction action, const util::HttpResponse& response) = 0;
        virtual void onPlaybackError(std::uint32_t seq, const PlaybackError& error) = 0;
        virtual void onRequestNextMedia(std::uint32_t seq) = 0;
        virtual void onPreparedNext(std::uint32_t seq, const AudioItem& item) = 0;
    };

    PlaybackEngineGeneric();
    ~PlaybackEngineGeneric() override;
    PlaybackEngineGeneric(const PlaybackEngineGeneric&) = delete;
    PlaybackEngineGeneric& operator=(const PlaybackEngineGeneric&) = delete;

    // connect(): player->connect(this), keep a weak ref to the delegate, start HTTP workers.
    // The engine must be owned by a shared_ptr (make_shared).
    virtual int connect(const std::shared_ptr<IMediaPlayer>& player, const std::shared_ptr<Delegate>& delegate);
    // disconnect(): cancel the pending fetch, stop workers, player->disconnect(), drop
    // player/delegate.
    virtual int disconnect();
    // reset(): Generic: stop(). Tidal: cancel the fetch + player stop, keeping mediaSeqNo.
    virtual int reset();
    // prepare(url): player->prepare({url}) then onPrepared(seq) synchronously.
    virtual int prepare(std::uint32_t seq, const std::string& url);
    // prepare(mediaId): resolve it through ServerInfo (async), then player->prepare + onPrepared,
    // or onPrepareError. mediaType 1 = video, otherwise track.
    virtual int prepare(std::uint32_t seq, const ServerInfo& serverInfo, const std::string& mediaId, unsigned mediaType);
    // prepareNext(): as prepare, for the gapless next item. Rejected (returns 1, logs "old
    // media_seqno") unless seq equals the current mediaSeqNo.
    virtual int prepareNext(std::uint32_t seq, const std::string& url);
    virtual int prepareNext(std::uint32_t seq, const ServerInfo& serverInfo, const std::string& mediaId, unsigned mediaType);
    virtual int play();
    virtual int pause();
    // stop(): mediaSeqNo = 0, cancel the fetch, player->stop() (blocking), then
    // delegate->onStatusUpdated({Idle}, {}) and onProgressUpdated(0, 0).
    virtual int stop();
    virtual int seekTo(std::uint64_t positionMs);
    virtual int clearToken();                      // no-op here
    // TCNG always verifies TLS; false is ignored with a warning.
    virtual int enableServerVerification(bool enable);
    virtual int interrupt(bool on);                // -> player->interrupt (no-op)

    std::uint32_t currentSeq() const { return seq_.load(); }

    // IMediaPlayer::Delegate: forward to the controller.
    void onStatusUpdated(const PlayStatus& status, const AudioItem& item) override;
    void onProgressUpdated(std::uint64_t positionMs, std::uint64_t durationMs) override;
    void onAudioFormatUpdated(const AudioFormat& format) override;
    void onPlaybackCompleted(bool hasNext) override;
    void onPlaybackError(const PlaybackError& error) override;
    void onRequestNextMedia() override;

    // AsyncHttpClient::Delegate: the generic URL fetch result.
    void onReceived(std::uint32_t id, const util::HttpResponse& response) override;

    // The generic ServerInfo fetch: GET serverUrl?media_id=<mediaId> with httpHeaderFields
    // (requestUrlFetch). Exposed for tests.
    static util::HttpRequest buildUrlFetchRequest(const ServerInfo& serverInfo, const std::string& mediaId);

protected:
    std::shared_ptr<Delegate> lockDelegate() const;
    // Notifies the controller that playback stopped (end of stop()/reset()).
    void notifyStopped(const std::shared_ptr<Delegate>& delegate);
    void requestUrlFetch(const ServerInfo& serverInfo, const std::string& mediaId, bool isNext);

    std::atomic<std::uint32_t> seq_{0};          // current mediaSeqNo
    mutable std::mutex mutex_;
    std::uint32_t pendingTaskId_ = 0;            // 0 = none
    bool pendingIsNext_ = false;
    mutable std::mutex delegateMutex_;           // guards delegate_ only (never held across calls)
    std::weak_ptr<Delegate> delegate_;
    std::shared_ptr<IMediaPlayer> player_;

private:
    std::shared_ptr<util::AsyncHttpClient> http_;      // AsyncHttpClient(1)
};

}  // namespace tcng::media
