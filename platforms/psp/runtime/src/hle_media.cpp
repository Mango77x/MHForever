// sceAudio (movies: hle_mpeg.cpp; sceUtility dialogs: hle_utility.cpp; sceSasCore: hle_sas.cpp; sceAtrac3plus:
// hle_atrac.cpp).
//
// The audio channels pace themselves in real time and hand the mixer what the game sends; the guest's own mixing,
// voices and music are elsewhere. Savedata requests are carried out for real by hle_savedata.cpp.

#include "hle.hpp"
#include "kernel.hpp"

#include "mhf/psp/host.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace mhf::psp::hle {

namespace {

Kernel &k(Runtime &rt) { return rt.kernel(); }

// ---------------------------------------------------------------- sceAudio

constexpr int kChannels = 8;
constexpr std::uint32_t kSampleRate = 44100;

struct AudioChannel {
    bool reserved{};
    std::uint32_t samples{};
    std::uint32_t format{}; // 0 stereo, 0x10 mono
    std::uint64_t busy_until{};
    std::deque<std::int16_t> queue; // interleaved stereo waiting for the host mixer
    bool primed{};                  // the host mixer is consuming this channel (see mix_audio)
};
std::array<AudioChannel, kChannels> g_channels{};
constexpr std::size_t kMaxQueuedFrames = 44100 / 4; // 250 ms per channel

void enqueue_pcm(Runtime &rt, AudioChannel &c, std::uint32_t buffer, std::uint32_t left_volume,
                 std::uint32_t right_volume) {
    if (buffer == 0) return;
    const bool mono = (c.format & 0x10u) != 0;
    auto &mem = rt.memory();
    for (std::uint32_t i = 0; i < c.samples; ++i) {
        std::int32_t l, r;
        if (mono) {
            l = r = static_cast<std::int16_t>(mem.load16(buffer + i * 2u));
        } else {
            l = static_cast<std::int16_t>(mem.load16(buffer + i * 4u));
            r = static_cast<std::int16_t>(mem.load16(buffer + i * 4u + 2u));
        }
        c.queue.push_back(static_cast<std::int16_t>((l * static_cast<std::int32_t>(left_volume)) >> 15));
        c.queue.push_back(static_cast<std::int16_t>((r * static_cast<std::int32_t>(right_volume)) >> 15));
    }
    while (c.queue.size() > kMaxQueuedFrames * 2) {
        c.queue.pop_front();
        c.queue.pop_front();
    }
}

void AudioChReserve(Runtime &, Context &ctx) {
    int ch = sarg(ctx, 0);
    if (ch < 0) {
        for (int i = kChannels - 1; i >= 0; --i) {
            if (!g_channels[static_cast<std::size_t>(i)].reserved) {
                ch = i;
                break;
            }
        }
    }
    if (ch < 0 || ch >= kChannels || g_channels[static_cast<std::size_t>(ch)].reserved) {
        ret(ctx, 0x80260002u); // SCE_AUDIO_ERROR_NO_CHANNELS_AVAILABLE
        return;
    }
    auto &c = g_channels[static_cast<std::size_t>(ch)];
    c.reserved = true;
    c.samples = arg(ctx, 1);
    c.format = arg(ctx, 2);
    c.busy_until = 0;
    ret(ctx, static_cast<std::uint32_t>(ch));
}

void AudioChRelease(Runtime &, Context &ctx) {
    const int ch = sarg(ctx, 0);
    if (ch >= 0 && ch < kChannels) g_channels[static_cast<std::size_t>(ch)].reserved = false;
    ret(ctx, 0);
}

void output_blocking(Runtime &rt, Context &ctx) {
    const int ch = sarg(ctx, 0);
    if (ch < 0 || ch >= kChannels || !g_channels[static_cast<std::size_t>(ch)].reserved) {
        ret(ctx, 0x80260003u);
        return;
    }
    auto &c = g_channels[static_cast<std::size_t>(ch)];
    const std::uint64_t now = k(rt).now_us();
    const std::uint64_t duration = static_cast<std::uint64_t>(c.samples) * 1'000'000u / kSampleRate;
    // `busy_until` is when everything queued so far will have played. The game may run up to kLead ahead of real
    // time (the hardware queues a block too), and after a short stall it catches up without blocking, so the output
    // stream keeps its real-time rate; after a long pause it restarts from now.
    constexpr std::uint64_t kLead = 20'000, kResync = 100'000;
    if (c.busy_until + kResync < now) c.busy_until = now;
    if (c.busy_until > now + kLead) k(rt).wait(Kernel::WaitType::Delay, 0, c.busy_until - kLead);
    c.busy_until += duration;
    ret(ctx, c.samples);
}

// (channel, left volume, right volume, buffer)
void output_panned_blocking(Runtime &rt, Context &ctx) {
    const int ch = sarg(ctx, 0);
    if (ch >= 0 && ch < kChannels && g_channels[static_cast<std::size_t>(ch)].reserved) {
        enqueue_pcm(rt, g_channels[static_cast<std::size_t>(ch)], arg(ctx, 3), arg(ctx, 1), arg(ctx, 2));
    }
    output_blocking(rt, ctx);
}

// (channel, volume, buffer)
void output_mono_volume_blocking(Runtime &rt, Context &ctx) {
    const int ch = sarg(ctx, 0);
    if (ch >= 0 && ch < kChannels && g_channels[static_cast<std::size_t>(ch)].reserved) {
        enqueue_pcm(rt, g_channels[static_cast<std::size_t>(ch)], arg(ctx, 2), arg(ctx, 1), arg(ctx, 1));
    }
    output_blocking(rt, ctx);
}

void AudioSetChannelDataLen(Runtime &, Context &ctx) {
    const int ch = sarg(ctx, 0);
    if (ch >= 0 && ch < kChannels) g_channels[static_cast<std::size_t>(ch)].samples = arg(ctx, 1);
    ret(ctx, 0);
}

void AudioGetChannelRestLength(Runtime &rt, Context &ctx) {
    const int ch = sarg(ctx, 0);
    if (ch < 0 || ch >= kChannels) {
        ret(ctx, 0);
        return;
    }
    const auto &c = g_channels[static_cast<std::size_t>(ch)];
    const std::uint64_t now = k(rt).now_us();
    ret(ctx, c.busy_until > now ? static_cast<std::uint32_t>((c.busy_until - now) * kSampleRate / 1'000'000u) : 0u);
}

void AudioZero(Runtime &, Context &ctx) { ret(ctx, 0); }

const HleFunction kFunctions[] = {
    {"sceAudio", 0x5EC81C55u, "sceAudioChReserve", &AudioChReserve},
    {"sceAudio", 0x6FC46853u, "sceAudioChRelease", &AudioChRelease},
    {"sceAudio", 0x13F592BCu, "sceAudioOutputPannedBlocking", &output_panned_blocking},
    {"sceAudio", 0x136CAF51u, "sceAudioOutputBlocking", &output_mono_volume_blocking},
    {"sceAudio", 0xCB2E439Eu, "sceAudioSetChannelDataLen", &AudioSetChannelDataLen},
    {"sceAudio", 0xB011922Fu, "sceAudioGetChannelRestLength", &AudioGetChannelRestLength},
    {"sceAudio", 0xE9D97901u, "sceAudioGetChannelRestLen", &AudioGetChannelRestLength},
    {"sceAudio", 0x95FD0C2Du, "sceAudioChangeChannelConfig", &AudioZero},
    {"sceAudio", 0xB7E1D8E7u, "sceAudioChangeChannelVolume", &AudioZero},
};

} // namespace

std::span<const HleFunction> media_functions() { return kFunctions; }

} // namespace mhf::psp::hle

namespace mhf::psp {

std::size_t mix_audio(Runtime &, std::int16_t *out, std::size_t frames) {
    // Each channel is a real-time stream that the game fills in blocks (256 frames for the voice mixer, 2048 for
    // decoded music). A channel is only consumed once it holds two blocks, so the arrival jitter of its blocks never
    // reaches the output; when it runs dry it is paused and refills instead of clicking.
    for (std::size_t i = 0; i < frames; ++i) {
        std::int32_t left = 0, right = 0;
        for (auto &c : hle::g_channels) {
            const std::size_t prebuffer = std::max<std::size_t>(2u * c.samples, 1024u);
            if (!c.primed && c.queue.size() / 2 >= prebuffer) c.primed = true;
            if (!c.primed) continue;
            if (c.queue.size() < 2) {
                c.primed = false;
                continue;
            }
            left += c.queue.front();
            c.queue.pop_front();
            right += c.queue.front();
            c.queue.pop_front();
        }
        out[i * 2] = static_cast<std::int16_t>(std::clamp(left, -32768, 32767));
        out[i * 2 + 1] = static_cast<std::int16_t>(std::clamp(right, -32768, 32767));
    }
    // Debugging: MHF_AUDIO_DUMP=<file> records the mixed output as raw 16-bit stereo at 44.1 kHz.
    static std::FILE *dump = [] {
        const char *path = std::getenv("MHF_AUDIO_DUMP");
        return path != nullptr ? std::fopen(path, "wb") : nullptr;
    }();
    if (dump != nullptr) {
        std::fwrite(out, sizeof(std::int16_t), frames * 2, dump);
        std::fflush(dump);
    }
    return frames;
}

} // namespace mhf::psp
