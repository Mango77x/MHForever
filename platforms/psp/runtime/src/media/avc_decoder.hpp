#pragma once

// The H.264 pictures inside the game's movies, decoded with FFmpeg's libavcodec (LGPL, see THIRD_PARTY.md and
// cmake/ffmpeg.cmake). One access unit in, at most one picture out, with no threads: the guest waits for each picture
// before it asks for the next (docs/05-runtime-hle.md).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace mhf::psp::media {

// A decoded picture as planar 4:2:0, tightly packed (the layout the PSP's sceMpeg produces).
struct Picture {
    std::uint32_t width{};
    std::uint32_t height{};
    bool full_range{}; // BT.601 full range (the movies use it) instead of limited
    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> cb;
    std::vector<std::uint8_t> cr;
};

class AvcDecoder {
public:
    AvcDecoder();
    ~AvcDecoder();
    AvcDecoder(const AvcDecoder &) = delete;
    AvcDecoder &operator=(const AvcDecoder &) = delete;

    // Whether this build has a decoder at all.

    // Starts a stream over; the first access unit should hold the parameter sets (the movies put them there).
    bool open();
    void close();
    [[nodiscard]] bool is_open() const noexcept;

    // Decodes one Annex B access unit. Returns true and fills `picture` when a picture comes out of it; a decoder may hold
    // a picture back until it sees the next unit.
    bool decode(std::span<const std::uint8_t> unit, Picture &picture);
    // At the end of the stream: returns a picture the decoder still held, if any.
    bool drain(Picture &picture);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mhf::psp::media
