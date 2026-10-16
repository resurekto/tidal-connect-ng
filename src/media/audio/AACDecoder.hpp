#pragma once
// AACDecoder: MP4/AAC demuxed by libavformat through a custom AVIO on the IInputStream,
// decoded by libavcodec and converted by libswresample to interleaved S16.
//
// Output is always stereo (swresample downmixes/upmixes): the sink and MediaPlayer assume
// 2 channels.

#include <cstdint>

#include "media/audio/Interfaces.hpp"

struct AVFormatContext;
struct AVIOContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace tcng::media {

class AACDecoder : public IAudioDecoder {
public:
    AACDecoder(IInputStream& input, IAudioOutput& output);
    ~AACDecoder() override;
    AACDecoder(const AACDecoder&) = delete;
    AACDecoder& operator=(const AACDecoder&) = delete;

    const AudioMetadata& metadata() const override { return metadata_; }
    DecoderStatus parseMetadata() override;
    DecoderStatus decode(unsigned frames) override;
    DecoderStatus seek(std::uint64_t frame) override;
    unsigned outputFrameSize() const override;
    bool ended() const override { return ended_; }
    void skipFrames(std::uint64_t frames) override { skip_ = frames; }
    std::uint64_t totalFrames() const override { return totalFrames_; }

private:
    static int readPacket(void* opaque, std::uint8_t* buf, int size);
    static std::int64_t seekStream(void* opaque, std::int64_t offset, int whence);

    void resetFormatContext();
    void closeCodec();
    // Decodes all frames the codec has ready; returns frames appended.
    std::uint64_t drainFrames();

    IInputStream& input_;
    IAudioOutput& output_;
    AudioMetadata metadata_;
    AVFormatContext* format_ = nullptr;
    AVIOContext* avio_ = nullptr;
    bool opened_ = false;
    AVCodecContext* codec_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    int streamIndex_ = -1;
    bool ended_ = false;
    std::uint64_t totalFrames_ = 0;
    std::uint64_t skip_ = 0;  // frames still to drop (skipFrames)
};

}  // namespace tcng::media
