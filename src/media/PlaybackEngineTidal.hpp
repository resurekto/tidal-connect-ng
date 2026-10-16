#pragma once
// PlaybackEngineTidal: the engine registered under the appId "tidal".
//
// prepare/prepareNext(ServerInfo, mediaId) post a fetch task (TidalUrlFetchTask, or with
// TCNG's --python-tidal-path a PythonTidalFetchTask; see ITrackFetchTask.hpp) to a private 1-thread
// ThreadPool; at most one fetch is outstanding (a new one cancels the previous, and a late
// result whose task id is no longer current is dropped: "url fetch discard"). On HTTP 200
// the result goes to player->prepare/prepareNext and delegate->onPrepared/onPreparedNext;
// otherwise delegate->onPrepareError/onPrepareNextError(seq, checkStatusCode(resp), resp).
// The token-switched access token is cached across fetches until clearToken().

#include <cstdint>
#include <memory>
#include <string>

#include "media/ITrackFetchTask.hpp"
#include "media/PlaybackEngineGeneric.hpp"
#include "util/ThreadPool.hpp"

namespace tcng::media {

class PlaybackEngineTidal : public PlaybackEngineGeneric, public util::ThreadPool::Delegate {
public:
    using Delegate = PlaybackEngineGeneric::Delegate;

    // `fetchTasks` null: TidalUrlFetchTaskFactory (built-in).
    explicit PlaybackEngineTidal(std::string clientId, std::shared_ptr<const ITrackFetchTaskFactory> fetchTasks = nullptr);
    ~PlaybackEngineTidal() override;

    int connect(const std::shared_ptr<IMediaPlayer>& player, const std::shared_ptr<Delegate>& delegate) override;
    int disconnect() override;
    int reset() override;
    int prepare(std::uint32_t seq, const ServerInfo& serverInfo, const std::string& mediaId, unsigned mediaType) override;
    int prepareNext(std::uint32_t seq, const ServerInfo& serverInfo, const std::string& mediaId, unsigned mediaType) override;
    int stop() override;
    int clearToken() override;
    int enableServerVerification(bool enable) override;
    using PlaybackEngineGeneric::prepare;
    using PlaybackEngineGeneric::prepareNext;

    // ThreadPool::Delegate: runs on the pool thread.
    void onCompleted(const std::shared_ptr<util::ThreadPool::ITask>& task) override;

    // checkStatusCode without side effects: HTTP 400-499 with JSON
    // sub_status/subStatus 4005 or 4007 -> SkipTrack; 400-499 with 11001-11003 ->
    // Default and *clearToken = true; anything else -> Default.
    static RecoveryAction classifyStatus(const util::HttpResponse& response, bool* clearToken);

private:
    RecoveryAction checkStatusCode(const util::HttpResponse& response);
    void sendTidalHttpRequest(const ServerInfo& serverInfo, const std::string& mediaId, bool isNext, unsigned type);
    void cancelPending();

    const std::string clientId_;         // --clientid
    const std::shared_ptr<const ITrackFetchTaskFactory> fetchTasks_;  // TCNG
    std::shared_ptr<util::ThreadPool> pool_;   // 1 worker
    std::string switchedToken_;          // guarded by mutex_
    AudioItem result_;
};

}  // namespace tcng::media
