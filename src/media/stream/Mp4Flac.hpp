#pragma once
// TCNG: FLAC out of fragmented MP4 ("Encapsulation of FLAC in ISO-BMFF"), for the DASH segments
// of --python-tidal-path. Every MP4 sample is one complete FLAC frame, so a native FLAC stream is
//   "fLaC" + the metadata blocks of the init segment's dfLa box
//   + the mdat payloads of each media segment, in order,
// which FLACDecoder (libFLAC) plays as is.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace tcng::media {

// Init segment (ftyp + moov) -> "fLaC" + metadata blocks (last-block flag fixed up);
// nullopt if there is no well-formed dfLa box.
std::optional<std::vector<char>> headerFromInit(const char* data, std::size_t size);

// Media segment (styp/sidx/moof/mdat...) -> the concatenated mdat payloads. Empty if the
// segment has no mdat.
std::vector<char> framesFromSegment(const char* data, std::size_t size);

// framesFromSegment for a segment that arrives in pieces: feed() the bytes in order and get
// the mdat payload bytes they complete. Box headers may be split across pieces.
class MdatStream {
public:
    // Appends the mdat payload bytes found in [data, data + size) to `out`.
    void feed(const char* data, std::size_t size, std::vector<char>& out);
    void reset() { *this = MdatStream{}; }
    // A malformed box size was seen; the rest of the segment is ignored.
    bool failed() const { return failed_; }

private:
    std::vector<std::uint8_t> header_;  // partial box header
    std::uint64_t remaining_ = 0;       // payload bytes left in the current box
    bool inBox_ = false;
    bool toEnd_ = false;                // size 0: the box runs to the end of the segment
    bool mdat_ = false;
    bool failed_ = false;
};

}  // namespace tcng::media
