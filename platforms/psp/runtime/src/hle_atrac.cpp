// sceAtrac3plus: ATRAC3 / ATRAC3+ music decoding (docs/05-runtime-hle.md §5g).
//
// The game hands over a whole RIFF/WAVE ".at3" file in memory (sceAtracSetDataAndGetID) and then pulls one frame of
// 16-bit stereo PCM per sceAtracDecodeData call (2048 samples for ATRAC3+, 1024 for ATRAC3). Frames are decoded by
// FFmpeg's decoders (third_party/at3_standalone, LGPL-2.1-or-later).
//
// Sample positions: the decoder's output starts with a fixed delay (0x170 samples for ATRAC3+, 0x45 for ATRAC3) plus the
// encoder offset stored in the "fact" chunk; the game sees a timeline that starts after both. Loop points come from the
// "smpl" chunk. These conventions were cross-checked against PPSSPP's implementation (GPL-2.0-or-later).
// Streaming (sceAtracAddStreamData) is not supported yet: the data must be entirely in memory.

#include "hle.hpp"

#include "at3_decoders.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace mhf::psp::hle {

namespace {

constexpr std::uint32_t kErrorNoAtracId = 0x80630003u;
constexpr std::uint32_t kErrorBadAtracId = 0x80630005u;
constexpr std::uint32_t kErrorUnknownFormat = 0x80630006u;
constexpr std::uint32_t kErrorSizeTooSmall = 0x80630011u;
constexpr std::uint32_t kErrorNoLoopInformation = 0x80630021u;
constexpr std::uint32_t kErrorAllDataDecoded = 0x80630024u;
constexpr int kMaxIds = 6;

std::uint32_t read32(const std::vector<std::uint8_t> &b, std::size_t at) {
    if (at + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8u |
           static_cast<std::uint32_t>(b[at + 2]) << 16u | static_cast<std::uint32_t>(b[at + 3]) << 24u;
}
std::uint16_t read16(const std::vector<std::uint8_t> &b, std::size_t at) {
    if (at + 2 > b.size()) return 0;
    return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8u);
}

std::int16_t to_pcm(float f) {
    if (f >= 1.0f) return 32767;
    if (f <= -1.0f) return -32767;
    return static_cast<std::int16_t>(f * 32767.0f);
}

class Track {
public:
    ~Track() {
        if (at3p_ != nullptr) atrac3p_free(at3p_);
        if (at3_ != nullptr) atrac3_free(at3_);
    }

    // Parses the RIFF header. Returns 0 or an sceAtrac error code.
    std::uint32_t open(psprecomp::GuestMemory &mem, std::uint32_t buffer, std::uint32_t size) {
        if (size < 72) return kErrorSizeTooSmall;
        file_.resize(size);
        for (std::uint32_t i = 0; i < size; ++i) file_[i] = mem.load8(buffer + i);
        buffer_ = buffer;
        if (read32(file_, 0) != 0x46464952u || read32(file_, 8) != 0x45564157u) return kErrorUnknownFormat; // RIFF, WAVE
        file_size_ = read32(file_, 4) + 8u;
        std::size_t at = 12;
        std::uint32_t fact_samples = 0, fact_offset = 0;
        int offset_adjust = 0;
        bool have_fmt = false, have_data = false, have_loop = false;
        std::uint32_t smpl_start = 0, smpl_end = 0;
        std::uint32_t data_size = 0;
        std::uint16_t joint_stereo = 0;
        while (at + 8 <= file_.size() && !have_data) {
            const std::uint32_t magic = read32(file_, at);
            std::uint32_t chunk = read32(file_, at + 4);
            chunk += chunk & 1u;
            at += 8;
            switch (magic) {
            case 0x20746D66u: { // "fmt "
                const std::uint16_t tag = read16(file_, at);
                if (tag == 0xFFFEu) plus_ = true;
                else if (tag == 0x0270u) plus_ = false;
                else return kErrorUnknownFormat;
                channels_ = read16(file_, at + 2);
                if (channels_ != 1 && channels_ != 2) return kErrorUnknownFormat;
                bytes_per_frame_ = read16(file_, at + 12);
                if (bytes_per_frame_ == 0) return kErrorUnknownFormat;
                if (!plus_) joint_stereo = read16(file_, at + 24);
                have_fmt = true;
                break;
            }
            case 0x74636166u: // "fact"
                fact_samples = read32(file_, at);
                if (chunk >= 8) fact_offset = read32(file_, at + 4);
                if (chunk >= 12) offset_adjust = static_cast<int>(fact_offset) - static_cast<int>(read32(file_, at + 8));
                break;
            case 0x6C706D73u: // "smpl"
                if (chunk >= 36 + 24 && read32(file_, at + 28) > 0) {
                    smpl_start = read32(file_, at + 36 + 8);
                    smpl_end = read32(file_, at + 36 + 12);
                    have_loop = smpl_start < smpl_end;
                }
                break;
            case 0x61746164u: // "data"
                data_offset_ = static_cast<std::uint32_t>(at);
                data_size = chunk;
                have_data = true;
                break;
            default: break;
            }
            if (!have_data) at += chunk;
        }
        if (!have_fmt || !have_data) return kErrorUnknownFormat;
        samples_per_frame_ = plus_ ? 2048 : 1024;
        const int extra = plus_ ? 0x170 : 0x45;
        full_offset_ = extra + static_cast<int>(fact_offset);
        data_end_ = std::min<std::uint32_t>(data_offset_ + data_size, size);
        int end = static_cast<int>(fact_samples);
        if (end <= 0) end = static_cast<int>(data_size / bytes_per_frame_) * samples_per_frame_ - full_offset_;
        end_sample_ = end - 1;
        if (have_loop) {
            loop_start_ = static_cast<int>(smpl_start) + extra + offset_adjust - full_offset_;
            loop_end_ = static_cast<int>(smpl_end) + extra + offset_adjust - full_offset_;
            if (loop_end_ > end_sample_) loop_end_ = end_sample_;
        }

        int block_align = bytes_per_frame_;
        if (plus_) {
            at3p_ = atrac3p_alloc(channels_, &block_align);
        } else {
            std::uint8_t extra_data[14]{};
            extra_data[0] = 1;
            extra_data[3] = static_cast<std::uint8_t>(channels_ << 3);
            extra_data[6] = static_cast<std::uint8_t>(joint_stereo);
            extra_data[8] = static_cast<std::uint8_t>(joint_stereo);
            extra_data[10] = 1;
            at3_ = atrac3_alloc(channels_, &block_align, extra_data, sizeof extra_data);
        }
        if (at3p_ == nullptr && at3_ == nullptr) return kErrorUnknownFormat;
        return 0;
    }

    int samples_per_frame() const { return samples_per_frame_; }
    int end_sample() const { return end_sample_; }
    int loop_start() const { return loop_start_; }
    int loop_end() const { return loop_end_; }
    int position() const { return position_; }
    bool finished() const { return finished_; }
    int loop_num() const { return loop_num_; }
    void set_loop_num(int n) { loop_num_ = n; }
    std::uint32_t buffer() const { return buffer_; }
    std::uint32_t file_size() const { return file_size_; }

    int bitrate() const {
        int rate = bytes_per_frame_ * 352800 / 1000;
        return plus_ ? ((rate >> 11) + 8) & 0xFFFFFFF0 : (rate + 511) >> 10;
    }

    // Decodes up to one frame of the game's timeline into `out` (stereo 16-bit), scaled by the player's music volume
    // (0..100). Returns the samples written.
    int decode(psprecomp::GuestMemory &mem, std::uint32_t out, int volume) {
        if (finished_) return 0;
        const bool looping = loop_num_ != 0 && loop_end_ >= 0;
        const int last = looping ? loop_end_ : end_sample_;
        const int count = std::clamp(last - position_ + 1, 0, samples_per_frame_);
        for (int i = 0; i < count; ++i) {
            std::int16_t left = 0, right = 0;
            sample(position_ + i + full_offset_, left, right);
            if (volume < 100) {
                left = static_cast<std::int16_t>(left * volume / 100);
                right = static_cast<std::int16_t>(right * volume / 100);
            }
            if (out != 0) {
                mem.store16(out + static_cast<std::uint32_t>(i) * 4u, static_cast<std::uint16_t>(left));
                mem.store16(out + static_cast<std::uint32_t>(i) * 4u + 2u, static_cast<std::uint16_t>(right));
            }
        }
        position_ += count;
        if (position_ > last) {
            if (looping) {
                position_ = loop_start_;
                if (loop_num_ > 0) --loop_num_;
            } else {
                finished_ = true;
            }
        }
        return count;
    }

private:
    // Raw decoder output sample `raw` (counted from the first frame of the data chunk).
    void sample(int raw, std::int16_t &left, std::int16_t &right) {
        if (raw < pcm_start_ || raw >= pcm_start_ + pcm_count_) {
            const int frame = raw / samples_per_frame_;
            if (frame != next_frame_) {
                // Seek: restart the decoder one frame early so the overlap of the transform is primed.
                flush();
                next_frame_ = std::max(0, frame - 1);
                if (next_frame_ < frame) decode_frame();
            }
            decode_frame();
        }
        const int i = raw - pcm_start_;
        if (i < 0 || i >= pcm_count_) {
            left = right = 0;
            return;
        }
        left = pcm_[static_cast<std::size_t>(i) * 2u];
        right = pcm_[static_cast<std::size_t>(i) * 2u + 1u];
    }

    void flush() {
        if (at3p_ != nullptr) atrac3p_flush_buffers(at3p_);
        if (at3_ != nullptr) atrac3_flush_buffers(at3_);
    }

    void decode_frame() {
        const int frame = next_frame_++;
        pcm_start_ = frame * samples_per_frame_;
        pcm_count_ = samples_per_frame_;
        pcm_.assign(static_cast<std::size_t>(samples_per_frame_) * 2u, 0);
        const std::uint32_t at = data_offset_ + static_cast<std::uint32_t>(frame) * bytes_per_frame_;
        if (at + bytes_per_frame_ > data_end_) return; // past the data: silence
        float *channels[2] = {left_.data(), right_.data()};
        int produced = 0;
        const int result = plus_ ? atrac3p_decode_frame(at3p_, channels, &produced, &file_[at], bytes_per_frame_)
                                 : atrac3_decode_frame(at3_, channels, &produced, &file_[at], bytes_per_frame_);
        if (result < 0) return;
        produced = std::min(produced, samples_per_frame_);
        const float *right = channels_ == 2 ? right_.data() : left_.data();
        for (int i = 0; i < produced; ++i) {
            pcm_[static_cast<std::size_t>(i) * 2u] = to_pcm(left_[static_cast<std::size_t>(i)]);
            pcm_[static_cast<std::size_t>(i) * 2u + 1u] = to_pcm(right[i]);
        }
    }

    std::vector<std::uint8_t> file_;
    std::uint32_t buffer_{}, file_size_{}, data_offset_{}, data_end_{};
    bool plus_{true};
    int channels_{2}, bytes_per_frame_{}, samples_per_frame_{2048};
    int full_offset_{}, end_sample_{}, loop_start_{-1}, loop_end_{-1};
    int loop_num_{};
    int position_{};
    bool finished_{};
    ATRAC3PContext *at3p_{};
    ATRAC3Context *at3_{};
    std::array<float, 4096> left_{}, right_{};
    std::vector<std::int16_t> pcm_;
    int pcm_start_{-1}, pcm_count_{}, next_frame_{0};
};

std::array<std::unique_ptr<Track>, kMaxIds> g_tracks;

Track *track(Context &ctx) {
    const int id = sarg(ctx, 0);
    if (id < 0 || id >= kMaxIds) return nullptr;
    return g_tracks[static_cast<std::size_t>(id)].get();
}

void store(Runtime &rt, std::uint32_t address, std::uint32_t value) {
    if (address != 0) rt.memory().store32(address, value);
}

// sceAtracSetDataAndGetID(buffer, size)
void AtracSetDataAndGetID(Runtime &rt, Context &ctx) {
    int id = -1;
    for (int i = 0; i < kMaxIds; ++i) {
        if (!g_tracks[static_cast<std::size_t>(i)]) {
            id = i;
            break;
        }
    }
    if (id < 0) {
        ret(ctx, kErrorNoAtracId);
        return;
    }
    auto t = std::make_unique<Track>();
    const std::uint32_t error = t->open(rt.memory(), arg(ctx, 0), arg(ctx, 1));
    if (error != 0) {
        std::fprintf(stderr, "[hle] sceAtracSetDataAndGetID: unsupported data at %08X (%08X)\n", arg(ctx, 0), error);
        ret(ctx, error);
        return;
    }
    g_tracks[static_cast<std::size_t>(id)] = std::move(t);
    ret(ctx, static_cast<std::uint32_t>(id));
}

void AtracReleaseAtracID(Runtime &, Context &ctx) {
    const int id = sarg(ctx, 0);
    if (id < 0 || id >= kMaxIds || !g_tracks[static_cast<std::size_t>(id)]) {
        ret(ctx, kErrorBadAtracId);
        return;
    }
    g_tracks[static_cast<std::size_t>(id)].reset();
    ret(ctx, 0);
}

// sceAtracDecodeData(id, out, *samples, *finish, *remainFrame)
void AtracDecodeData(Runtime &rt, Context &ctx) {
    Track *t = track(ctx);
    if (t == nullptr) {
        ret(ctx, kErrorBadAtracId);
        return;
    }
    if (t->finished()) {
        store(rt, arg(ctx, 2), 0);
        store(rt, arg(ctx, 3), 1);
        ret(ctx, kErrorAllDataDecoded);
        return;
    }
    const int count = t->decode(rt.memory(), arg(ctx, 1), std::clamp(rt.options().music_volume, 0, 100));
    store(rt, arg(ctx, 2), static_cast<std::uint32_t>(count));
    store(rt, arg(ctx, 3), t->finished() ? 1u : 0u);
    store(rt, arg(ctx, 4), 0xFFFFFFFFu); // all the data is in memory
    ret(ctx, 0);
}

void AtracGetRemainFrame(Runtime &rt, Context &ctx) {
    if (track(ctx) == nullptr) {
        ret(ctx, kErrorBadAtracId);
        return;
    }
    store(rt, arg(ctx, 1), 0xFFFFFFFFu);
    ret(ctx, 0);
}

// (id, *endSample, *loopStartSample, *loopEndSample)
void AtracGetSoundSample(Runtime &rt, Context &ctx) {
    Track *t = track(ctx);
    if (t == nullptr) {
        ret(ctx, kErrorBadAtracId);
        return;
    }
    store(rt, arg(ctx, 1), static_cast<std::uint32_t>(t->end_sample()));
    store(rt, arg(ctx, 2), static_cast<std::uint32_t>(t->loop_start()));
    store(rt, arg(ctx, 3), static_cast<std::uint32_t>(t->loop_end()));
    ret(ctx, 0);
}

void AtracGetBitrate(Runtime &rt, Context &ctx) {
    Track *t = track(ctx);
    if (t == nullptr) {
        ret(ctx, kErrorBadAtracId);
        return;
    }
    store(rt, arg(ctx, 1), static_cast<std::uint32_t>(t->bitrate()));
    ret(ctx, 0);
}

// (id, *loopNum, *loopStatus)
void AtracGetLoopStatus(Runtime &rt, Context &ctx) {
    Track *t = track(ctx);
    if (t == nullptr) {
        ret(ctx, kErrorBadAtracId);
        return;
    }
    store(rt, arg(ctx, 1), static_cast<std::uint32_t>(t->loop_num()));
    store(rt, arg(ctx, 2), t->loop_start() >= 0 ? 1u : 0u);
    ret(ctx, 0);
}

void AtracSetLoopNum(Runtime &, Context &ctx) {
    Track *t = track(ctx);
    if (t == nullptr) {
        ret(ctx, kErrorBadAtracId);
        return;
    }
    if (t->loop_start() < 0) {
        ret(ctx, kErrorNoLoopInformation);
        return;
    }
    t->set_loop_num(sarg(ctx, 1));
    ret(ctx, 0);
}

void AtracGetNextDecodePosition(Runtime &rt, Context &ctx) {
    Track *t = track(ctx);
    if (t == nullptr) {
        ret(ctx, kErrorBadAtracId);
        return;
    }
    if (t->finished()) {
        ret(ctx, kErrorAllDataDecoded);
        return;
    }
    store(rt, arg(ctx, 1), static_cast<std::uint32_t>(t->position()));
    ret(ctx, 0);
}

// (id, *writePointer, *writableBytes, *readOffset): everything is already in memory, nothing can be written.
void AtracGetStreamDataInfo(Runtime &rt, Context &ctx) {
    Track *t = track(ctx);
    if (t == nullptr) {
        ret(ctx, kErrorBadAtracId);
        return;
    }
    store(rt, arg(ctx, 1), t->buffer());
    store(rt, arg(ctx, 2), 0);
    store(rt, arg(ctx, 3), t->file_size());
    ret(ctx, 0);
}

const HleFunction kFunctions[] = {
    {"sceAtrac3plus", 0x7A20E7AFu, "sceAtracSetDataAndGetID", &AtracSetDataAndGetID},
    {"sceAtrac3plus", 0x61EB33F5u, "sceAtracReleaseAtracID", &AtracReleaseAtracID},
    {"sceAtrac3plus", 0x6A8C3CD5u, "sceAtracDecodeData", &AtracDecodeData},
    {"sceAtrac3plus", 0x9AE849A7u, "sceAtracGetRemainFrame", &AtracGetRemainFrame},
    {"sceAtrac3plus", 0xA2BBA8BEu, "sceAtracGetSoundSample", &AtracGetSoundSample},
    {"sceAtrac3plus", 0xA554A158u, "sceAtracGetBitrate", &AtracGetBitrate},
    {"sceAtrac3plus", 0xFAA4F89Bu, "sceAtracGetLoopStatus", &AtracGetLoopStatus},
    {"sceAtrac3plus", 0x868120B5u, "sceAtracSetLoopNum", &AtracSetLoopNum},
    {"sceAtrac3plus", 0xE23E3A35u, "sceAtracGetNextDecodePosition", &AtracGetNextDecodePosition},
    {"sceAtrac3plus", 0x5D268707u, "sceAtracGetStreamDataInfo", &AtracGetStreamDataInfo},
};

} // namespace

std::span<const HleFunction> atrac_functions() { return kFunctions; }

} // namespace mhf::psp::hle
