#pragma once

// The ATRAC3+ sound of the movies, decoded by the standalone ATRAC3/ATRAC3+ decoder already in the tree
// (third_party/at3_standalone, from FFmpeg through PPSSPP; see THIRD_PARTY.md). It is the same decoder sceAtrac3plus
// uses for the game's music (hle_atrac.cpp), so there is only one implementation of the format in the runtime.
//
// A PSMF audio frame carries an 8-byte header of its own (0x0FD0 and the codec parameters) which the demultiplexer
// strips: what is handed to decode() is the raw ATRAC3+ frame, and what comes out is 2048 samples per channel.

#include <array>
#include <cstdint>
#include <span>

struct ATRAC3PContext;

namespace mhf::psp::media {

inline constexpr unsigned kAtracSamplesPerFrame = 2048;

class AtracDecoder {
public:
    AtracDecoder() = default;
    ~AtracDecoder();
    AtracDecoder(const AtracDecoder &) = delete;
    AtracDecoder &operator=(const AtracDecoder &) = delete;

    // `channels` and `bytes_per_frame` (the size of one frame without its PSMF header) come from the stream itself.
    bool open(unsigned channels, unsigned bytes_per_frame);
    void close();
    [[nodiscard]] bool is_open() const noexcept { return context_ != nullptr; }
    [[nodiscard]] unsigned channels() const noexcept { return channels_; }

    // Decodes one frame into `out` as interleaved 16-bit samples (kAtracSamplesPerFrame * channels). Returns false, and
    // leaves silence, when the frame cannot be decoded.
    bool decode(std::span<const std::uint8_t> frame, std::int16_t *out) noexcept;

private:
    ATRAC3PContext *context_{};
    unsigned channels_{2};
    std::array<float, 4096> left_{};
    std::array<float, 4096> right_{};
};

} // namespace mhf::psp::media
