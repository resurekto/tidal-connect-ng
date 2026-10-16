#pragma once
// Internal message ids, layout 0x00SSGGNN (SS = destination service, GG = group:
// 01 app command, 02 inter-service, 04 self-posted completion). 0x800002xx are
// broadcast ids several services understand.

#include <cstdint>

namespace tcng::core {

namespace service {
inline constexpr const char* clientcomm = "clientcomm";
inline constexpr const char* session = "session";
inline constexpr const char* queue = "queue";
inline constexpr const char* playback = "playback";
inline constexpr const char* speaker = "speaker";
inline constexpr const char* mdns = "mdns";
}  // namespace service

namespace msg {
// clientcomm
inline constexpr std::uint32_t ClientcommBlockPacket = 0x10201;
inline constexpr std::uint32_t ClientcommCloseSocket = 0x10202;
// session
inline constexpr std::uint32_t SessionStart = 0x20101;
inline constexpr std::uint32_t SessionEnd = 0x20102;
inline constexpr std::uint32_t SessionResume = 0x20103;
inline constexpr std::uint32_t SessionGrantResources = 0x20201;
inline constexpr std::uint32_t SessionRevokeResources = 0x20202;
inline constexpr std::uint32_t SessionLostConnection = 0x20203;
// queue
inline constexpr std::uint32_t QueueLoadMedia = 0x40101;
inline constexpr std::uint32_t QueueLoadLocalQueue = 0x40102;
inline constexpr std::uint32_t QueueLoadCloudQueue = 0x40103;
inline constexpr std::uint32_t QueueRespondQueueItems = 0x40104;
inline constexpr std::uint32_t QueuePrevious = 0x40105;
inline constexpr std::uint32_t QueueNext = 0x40106;
inline constexpr std::uint32_t QueueSelectQueueItem = 0x40107;
inline constexpr std::uint32_t QueueRefreshQueue = 0x40108;
inline constexpr std::uint32_t QueueSetRepeatMode = 0x40109;
inline constexpr std::uint32_t QueueSetShuffle = 0x4010a;
inline constexpr std::uint32_t QueueUpdateServerInfo = 0x4010b;
inline constexpr std::uint32_t QueuePurge = 0x40201;
inline constexpr std::uint32_t QueueNotifyMediaEnded = 0x40202;
inline constexpr std::uint32_t QueueRequestNextMedia = 0x40203;
inline constexpr std::uint32_t QueueNotifyTokenExpired = 0x40204;
inline constexpr std::uint32_t QueueNotifyLoaded = 0x40401;
inline constexpr std::uint32_t QueueNotifyLoadError = 0x40402;
inline constexpr std::uint32_t QueueNotifyQueueItemsChanged = 0x40403;
// playback
inline constexpr std::uint32_t PlaybackPlay = 0x80101;
inline constexpr std::uint32_t PlaybackPause = 0x80102;
inline constexpr std::uint32_t PlaybackStop = 0x80103;
inline constexpr std::uint32_t PlaybackSeek = 0x80104;
inline constexpr std::uint32_t PlaybackReset = 0x80201;
inline constexpr std::uint32_t PlaybackSetServerInfo = 0x80202;
inline constexpr std::uint32_t PlaybackSetMedia = 0x80203;
inline constexpr std::uint32_t PlaybackSetNextMedia = 0x80204;
inline constexpr std::uint32_t PlaybackInterruptSpeaker = 0x80205;
inline constexpr std::uint32_t PlaybackNotifyPrepared = 0x80401;
inline constexpr std::uint32_t PlaybackNotifyPreparedNext = 0x80402;
inline constexpr std::uint32_t PlaybackNotifyMpCompleted = 0x80403;
inline constexpr std::uint32_t PlaybackNotifyMpRequestNextMedia = 0x80404;
// speaker
inline constexpr std::uint32_t SpeakerSetVolume = 0x100101;
inline constexpr std::uint32_t SpeakerSetMute = 0x100102;
inline constexpr std::uint32_t SpeakerRequestResources = 0x100201;
inline constexpr std::uint32_t SpeakerReleaseResources = 0x100202;
inline constexpr std::uint32_t SpeakerNotifyAudioFormatUpdate = 0x100203;
inline constexpr std::uint32_t SpeakerRequestFromController = 0x100401;
// mdns
inline constexpr std::uint32_t MdnsSetConfig = 0x200201;
inline constexpr std::uint32_t MdnsStartServer = 0x200202;
inline constexpr std::uint32_t MdnsStopServer = 0x200203;
inline constexpr std::uint32_t MdnsChangeFriendlyName = 0x200204;
// broadcast
inline constexpr std::uint32_t SendPacket = 0x80000201;
inline constexpr std::uint32_t NotifySessionState = 0x80000202;
inline constexpr std::uint32_t RequestStatusNotification = 0x80000203;
}  // namespace msg

}  // namespace tcng::core
