#pragma once
// spkcon framing: FF 02 | u16 BE length | payload | FF 03.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace tcng::spkcon {

inline constexpr std::size_t kMaxPayload = 0xFFFF;

// Builds one frame. Returns nullopt if the payload does not fit the 16-bit length.
std::optional<std::string> encodeFrame(std::string_view payload);

// Incremental decoder; state persists across feed() calls, so partial frames and
// several frames per read are both handled.
class FrameDecoder {
public:
    using Handler = std::function<void(std::string payload)>;
    using ErrorHandler = std::function<void(const std::string& what)>;

    explicit FrameDecoder(Handler onFrame, ErrorHandler onError = {});

    void feed(std::string_view bytes);
    void reset();

private:
    enum class State { Start1, Start2, Len1, Len2, Payload, End1, End2 };

    void fail(const char* what, unsigned char byte);

    Handler onFrame_;
    ErrorHandler onError_;
    State state_ = State::Start1;
    std::size_t length_ = 0;
    std::string payload_;
};

}  // namespace tcng::spkcon
