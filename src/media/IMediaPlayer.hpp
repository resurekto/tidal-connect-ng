#pragma once
// IMediaPlayer (MediaPlayer) with its value types and listener.
//
// Every method returns 0 (kSuccess) unless noted; MediaPlayer never fails.

#include <cstdint>
#include <string>
#include <vector>

namespace tcng::media {

constexpr int kSuccess = 0;

// Player states, with the names reported to the app:
// 0 "IDLE", 1 "PLAYING", 2 "PAUSED", 3 "BUFFERING", other "INVALID".
enum class playstate_t : int { Idle = 0, Playing = 1, Paused = 2, Buffering = 3 };

inline const char* toString(playstate_t state) {
    switch (state) {
    case playstate_t::Idle: return "IDLE";
    case playstate_t::Playing: return "PLAYING";
    case playstate_t::Paused: return "PAUSED";
    case playstate_t::Buffering: return "BUFFERING";
    }
    return "INVALID";
}

// Track status codes. Only the values below have a known producer or meaning. The playback
// controller maps 8 -> 1012, 9 -> 1011 and everything else -> 1301.
namespace track_status {
constexpr int kDecoderError = 2;         // MediaPlayer::work_: decoder status 2/3
constexpr int kInvalidMedia = 4;         // controller: setMedia without srcUrl/mediaId
constexpr int kMaxConnections = 9;       // MediaPlayer::work_: fetcher gave up after 3 attempts
}  // namespace track_status

// AudioItem::mediaformat and audioCodec as produced by TidalUrlFetchTask from the
// manifest "codecs". MQA (6/7) and MPEG-H (3/8) are not supported by TCNG and
// map to unknown.
namespace media_format {
constexpr int kUnknown = 0;
constexpr int kAac = 3;   // mp4a.40.x in MP4
constexpr int kFlac = 6;
}  // namespace media_format
namespace audio_codec {
constexpr int kUnknown = 0;
constexpr int kAacLc = 3;  // mp4a.40.2
constexpr int kFlac = 4;
constexpr int kHeAac = 5;  // mp4a.40.5
}  // namespace audio_codec

// TCNG: one media segment of a segmented (DASH) stream.
struct AudioSegment {
    std::string url;
    double durationSeconds = 0.0;
};

// What a track resolves to.
struct AudioItem {
    int mediaformat = media_format::kUnknown;
    int audioCodec = audio_codec::kUnknown;
    int streamType = 0;                        // always 0
    std::string url;                           // stream URL (manifest urls[0])
    std::string keyId;                         // only for encryptionType "OLD_AES"; never used
    std::string customData;                    // {"audioMode","audioQuality"} JSON text
    // TCNG (--python-tidal-path): a segmented stream, played as initUrl + segments in order
    // (fragmented MP4). Empty for single-file items, which use `url`.
    std::string initUrl;
    std::vector<AudioSegment> segments;
    double durationSeconds = 0.0;  // TCNG: known track duration, 0 = from the decoder
};

// PlayStatus: just the player state.
struct PlayStatus {
    playstate_t state = playstate_t::Idle;
};

// AudioFormat: reported to the app and the speaker controller when the format changes.
// notifyAudioFormatUpdated {mediaformat, audioCodec, streamType, audioChannels,
// audioBitPerSample, audioSamplingRate, audioBitrate}.
struct AudioFormat {
    int mediaformat = 0;
    int audioCodec = 0;
    int streamType = 0;
    int audioChannels = 0;
    int audioBitPerSample = 0;
    int audioSamplingRate = 0;
    int audioBitrate = 0;  // rate * bits * channels
};

// PlaybackError: reported to the app as notifyPlaybackError.
struct PlaybackError {
    int trackStatus = 0;      // track_status::*
    int httpStatus = 0;       // -> details.subCode when != 0
    std::string description;  // -> details.description
};

class IMediaPlayer {
public:
    // IMediaPlayer::Delegate (implemented by PlaybackEngineGeneric). Called on the media
    // player's loop thread.
    class Delegate {
    public:
        virtual ~Delegate() = default;
        virtual void onStatusUpdated(const PlayStatus& status, const AudioItem& item) = 0;
        virtual void onProgressUpdated(std::uint64_t positionMs, std::uint64_t durationMs) = 0;
        virtual void onAudioFormatUpdated(const AudioFormat& format) = 0;
        virtual void onPlaybackCompleted(bool hasNext) = 0;
        virtual void onPlaybackError(const PlaybackError& error) = 0;
        virtual void onRequestNextMedia() = 0;
    };

    virtual ~IMediaPlayer() = default;
    virtual int init() = 0;                                 // no-op
    virtual int deinit() = 0;                               // no-op
    virtual int connect(Delegate* delegate) = 0;            // sets the listener
    virtual int disconnect() = 0;                           // clears it
    virtual int prepare(const AudioItem& item) = 0;         // stop + load; ends PAUSED
    virtual int prepareNext(const AudioItem& item) = 0;     // queue (gapless candidate)
    virtual int play() = 0;
    virtual int pause() = 0;
    virtual int stop() = 0;                                 // synchronous
    virtual int seekTo(std::uint64_t positionMs) = 0;
    virtual int getStatus(PlayStatus& status) = 0;
    virtual int interrupt(bool on) = 0;                     // no-op
};

}  // namespace tcng::media
