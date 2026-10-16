#include "spkcon/SpkconPacket.hpp"

#include <fmt/format.h>

namespace tcng::spkcon {

namespace {
constexpr unsigned char kStart1 = 0xFF;
constexpr unsigned char kStart2 = 0x02;
constexpr unsigned char kEnd1 = 0xFF;
constexpr unsigned char kEnd2 = 0x03;
}  // namespace

std::optional<std::string> encodeFrame(std::string_view payload) {
    if (payload.size() > kMaxPayload)
        return std::nullopt;
    std::string frame;
    frame.reserve(payload.size() + 6);
    frame.push_back(static_cast<char>(kStart1));
    frame.push_back(static_cast<char>(kStart2));
    frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
    frame.push_back(static_cast<char>(payload.size() & 0xFF));
    frame.append(payload);
    frame.push_back(static_cast<char>(kEnd1));
    frame.push_back(static_cast<char>(kEnd2));
    return frame;
}

FrameDecoder::FrameDecoder(Handler onFrame, ErrorHandler onError)
    : onFrame_(std::move(onFrame)), onError_(std::move(onError)) {}

void FrameDecoder::reset() {
    state_ = State::Start1;
    length_ = 0;
    payload_.clear();
}

void FrameDecoder::fail(const char* what, unsigned char byte) {
    if (onError_)
        onError_(fmt::format("data is not {}({})", what, byte));
    reset();
}

void FrameDecoder::feed(std::string_view bytes) {
    for (char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        switch (state_) {
        case State::Start1:
            if (b == kStart1)
                state_ = State::Start2;
            else
                fail("start marker1", b);
            break;
        case State::Start2:
            if (b == kStart2) {
                state_ = State::Len1;
            } else {
                fail("start marker2", b);
                // Resynchronise on this byte instead of dropping the rest of the read.
                if (b == kStart1)
                    state_ = State::Start2;
            }
            break;
        case State::Len1:
            length_ = static_cast<std::size_t>(b) << 8;
            state_ = State::Len2;
            break;
        case State::Len2:
            length_ |= b;
            payload_.clear();
            payload_.reserve(length_);
            state_ = length_ == 0 ? State::End1 : State::Payload;
            break;
        case State::Payload:
            payload_.push_back(c);
            if (payload_.size() >= length_)
                state_ = State::End1;
            break;
        case State::End1:
            if (b == kEnd1) {
                state_ = State::End2;
            } else {
                fail("end marker1", b);
            }
            break;
        case State::End2:
            if (b == kEnd2) {
                std::string payload;
                payload.swap(payload_);
                reset();
                if (onFrame_)
                    onFrame_(std::move(payload));
            } else {
                fail("end marker2", b);
                if (b == kStart1)
                    state_ = State::Start2;
            }
            break;
        }
    }
}

}  // namespace tcng::spkcon
