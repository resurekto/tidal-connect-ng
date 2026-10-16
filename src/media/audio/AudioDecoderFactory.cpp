#include "media/audio/AudioDecoderFactory.hpp"

#include <cstring>

#include "util/Log.hpp"
#include "media/audio/AACDecoder.hpp"
#include "media/audio/FLACDecoder.hpp"

namespace tcng::media {

// probeStream: compares against the FLAC magic "fLaC".
StreamFormat AudioDecoderFactory::probeStream(IInputStream& input) {
    log::get()->debug("AudioDecoderFactory::create - building decoder based on stream probe");
    char buf[4] = {};
    input.seek(0);
    const std::size_t n = input.read(buf, sizeof(buf));
    input.seek(0);
    return n == 4 && std::memcmp(buf, "fLaC", 4) == 0 ? StreamFormat::Flac : StreamFormat::Aac;
}

std::unique_ptr<IAudioDecoder> AudioDecoderFactory::create(IInputStream& input, IAudioOutput& output) {
    StreamFormat format = input.format();
    if (format == StreamFormat::None) {
        format = probeStream(input);
        input.setStreamFormat(format);
    }
    log::get()->debug("AudioDecoderFactory::create - got stream format {}", stringify(format));
    if (isAacFamily(format))
        return std::make_unique<AACDecoder>(input, output);
    if (format == StreamFormat::Flac)
        return std::make_unique<FLACDecoder>(input, output);
    return nullptr;
}

}  // namespace tcng::media
