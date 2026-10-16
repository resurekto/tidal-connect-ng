#pragma once
// AudioDecoderFactory: AAC family -> AACDecoder, FLAC -> FLACDecoder; an unknown format
// is probed from the first 4 bytes of the stream.

#include <memory>

#include "media/audio/Interfaces.hpp"

namespace tcng::media {

class AudioDecoderFactory {
public:
    virtual ~AudioDecoderFactory() = default;
    virtual std::unique_ptr<IAudioDecoder> create(IInputStream& input, IAudioOutput& output);

    // "fLaC" magic -> Flac, anything else -> Aac. Restores the stream position to 0.
    static StreamFormat probeStream(IInputStream& input);
};

}  // namespace tcng::media
