#pragma once

// The stream side of the game's movies. A ".pmo" file holds a PSMF: an MPEG program stream of 2048-byte packs, each
// carrying video (H.264, stream id 0xE0) and sound (ATRAC3+, private stream 1, 0xBD) payloads with their PES
// timestamps. This class turns those packs into access units: one picture per unit, delimited by the H.264 access unit
// delimiter, and one ATRAC3+ frame per audio unit.
//
// This is the same shape PPSSPP's MpegDemux and MediaEngine see, which is what makes the layout known: the packs start
// with 00 00 01 BA and the PES packets with 00 00 01 <id>.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace mhf::psp::media {

// One pack, which is also the unit of the ring buffer and of a disc sector.
inline constexpr std::size_t kPackSize = 2048;

// An ATRAC3+ frame in a PSMF audio stream: an 8-byte header (0x0FD0 and the codec parameters) before each frame, and
// 2048 samples per channel once decoded.
inline constexpr std::size_t kAtracFrameHeader = 8;
inline constexpr std::size_t kAtracFrameSamples = 2048;

struct AccessUnit {
    std::vector<std::uint8_t> data; // video: Annex B; audio: the ATRAC3+ frame without its 8-byte header
    std::int64_t pts{-1};           // 90 kHz
    std::int64_t dts{-1};
};

class PsmfDemuxer {
public:
    void reset();

    // Adds one pack. Returns false when it is not a program stream pack (the ring buffer is out of sync).
    bool push_pack(std::span<const std::uint8_t> pack);
    // No more packs will come: the last picture in the buffer is complete.
    void end_of_stream() { ended_ = true; split_video(); }

    // A picture is only handed out once the next one has started, since that is what marks its end.
    [[nodiscard]] bool video_ready() const noexcept { return !video_units_.empty(); }
    [[nodiscard]] bool audio_ready() const noexcept { return !audio_units_.empty(); }
    std::optional<AccessUnit> pop_video();
    std::optional<AccessUnit> pop_audio();

    // Size of one ATRAC3+ frame including its header, and its channel count, from the first frame header seen; 0 until
    // then. The decoder needs both, and only the stream carries them.
    [[nodiscard]] std::size_t audio_frame_size() const noexcept { return audio_frame_size_; }
    [[nodiscard]] unsigned audio_channels() const noexcept { return audio_channels_; }

    [[nodiscard]] std::uint32_t packs_demuxed() const noexcept { return packs_demuxed_; }

private:
    struct Stamp {
        std::size_t offset{}; // where in the elementary stream buffer the payload starts
        std::int64_t pts{-1};
        std::int64_t dts{-1};
    };

    void add_video(std::span<const std::uint8_t> payload, std::int64_t pts, std::int64_t dts);
    void add_audio(std::span<const std::uint8_t> payload, std::int64_t pts);
    void split_video();
    void split_audio();

    std::uint32_t packs_demuxed_{};

    std::vector<std::uint8_t> video_;
    std::size_t video_scanned_{};
    std::vector<std::size_t> video_starts_;
    std::deque<Stamp> video_stamps_;
    std::deque<AccessUnit> video_units_;
    std::int64_t last_video_pts_{-1};
    std::int64_t last_video_dts_{-1};
    bool ended_{};

    std::vector<std::uint8_t> audio_;
    std::deque<Stamp> audio_stamps_;
    std::deque<AccessUnit> audio_units_;
    std::int64_t last_audio_pts_{-1};
    std::size_t audio_frame_size_{};
    unsigned audio_channels_{};
};

// The 90 kHz duration of one picture at the stream's frame rate, from the PSMF header's rate code (0x24). Falls back to
// 29.97 Hz, the rate the movies use.
[[nodiscard]] std::int64_t frame_ticks(std::uint32_t frame_rate_code) noexcept;

} // namespace mhf::psp::media
