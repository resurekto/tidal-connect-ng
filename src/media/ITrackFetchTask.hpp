#pragma once
// TCNG: what PlaybackEngineTidal posts to its 1-thread ThreadPool to turn a TIDAL mediaId into
// an AudioItem. TCNG picks the implementation once, at startup, through the factory:
//   - TidalUrlFetchTaskFactory: token switch + playbackinfopostpaywall (built-in);
//   - PythonTidalFetchTaskFactory: python-tidal helper process (--python-tidal-path).

#include <memory>
#include <string>

#include "media/IMediaPlayer.hpp"
#include "media/PlaybackEngineGeneric.hpp"
#include "util/ThreadPool.hpp"
#include "util/HttpTypes.hpp"

namespace tcng::media {

struct TrackFetchRequest {
    ServerInfo serverInfo;
    std::string clientId;       // --clientid
    std::string mediaId;
    std::string switchedToken;  // cached by the engine; empty = switch first
    unsigned type = 0;          // 1 = video, otherwise track
};

class ITrackFetchTask : public util::ThreadPool::ITask {
public:
    // Valid after execute(): the AudioItem and the switched token for the engine to cache.
    virtual void getResult(AudioItem& item, std::string& switchedToken) const = 0;
    // Outcome as an HTTP response: status 200 = success; anything else goes through
    // PlaybackEngineTidal::checkStatusCode (sub_status 4005/4007 -> skip track).
    virtual const util::HttpResponse& response() const = 0;
};

class ITrackFetchTaskFactory {
public:
    virtual ~ITrackFetchTaskFactory() = default;
    virtual std::shared_ptr<ITrackFetchTask> create(TrackFetchRequest request) const = 0;
};

}  // namespace tcng::media
