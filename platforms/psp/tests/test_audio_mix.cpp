// The host mixer (mix_audio) against the channels' queues, without a game and without an audio device: blocks go in as
// the game's output calls queue them and the mixer is asked for what the frontend would ask it at every poll.
//
// Issue #148: the frontend steers the device's own queue by asking for a few frames less (or more) than the time that
// passed, so a device that plays a little slower than the system clock leaves the difference in the channels' queues.
// Nothing took it out again, and the sound fell further behind the picture the longer the game ran, up to the queues'
// 250 ms. A channel now never keeps much more than its pre-buffer: what is beyond that is late and is dropped.

#include "hle.hpp"

#include "mhf/psp/host.hpp"
#include "mhf/psp/runtime.hpp"

#include "check.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using mhf::psp::hle::audio_test_queue;
using mhf::psp::hle::audio_test_queued;
using mhf::psp::hle::audio_test_reserve;

constexpr double kRate = 44100.0;
constexpr double kPollSeconds = 0.005; // the frontend hands the device audio about every 5 ms

struct Run {
    std::size_t queued_at_end{};   // frames the channel holds when the run ends
    std::size_t largest_queued{};  // and the most it held after the first second
    std::size_t jumps{};           // places where the output skipped part of the stream
};

// One channel filled in blocks of `block` frames at the real rate for `seconds`, the mixer asked for `speed` times the
// real rate (below 1: a device that plays slower than the system clock). Every sample is its frame's number (counting
// 1 to 32767 over and over, 0 being silence), so a skip in the output shows as a jump.
Run run(mhf::psp::Runtime &rt, std::uint32_t block, double seconds, double speed) {
    constexpr int kChannel = 3;
    audio_test_reserve(kChannel, block);
    Run out;
    std::uint32_t next_frame = 0;
    double produced = 0.0, consumed = 0.0;
    std::int32_t last = -1;
    std::vector<std::int16_t> blockdata(static_cast<std::size_t>(block) * 2), mixed;
    const auto polls = static_cast<std::size_t>(seconds / kPollSeconds);
    for (std::size_t p = 0; p < polls; ++p) {
        produced += kPollSeconds * kRate;
        while (next_frame + block <= produced) { // the game's blocks that are due by now
            for (std::uint32_t i = 0; i < block; ++i) {
                const auto v = static_cast<std::int16_t>((next_frame + i) % 32767u + 1u); // 1..32767: 0 is silence
                blockdata[i * 2] = v;
                blockdata[i * 2 + 1] = v;
            }
            audio_test_queue(kChannel, blockdata.data(), block);
            next_frame += block;
        }
        consumed += kPollSeconds * kRate * speed;
        const auto frames = static_cast<std::size_t>(consumed);
        consumed -= static_cast<double>(frames);
        mixed.assign(frames * 2, 0);
        mhf::psp::mix_audio(rt, mixed.data(), frames);
        for (std::size_t i = 0; i < frames; ++i) {
            const std::int32_t v = mixed[i * 2];
            if (v == 0 && last <= 0) continue; // silence before the channel starts (or while it refills)
            if (last > 0 && v != 0 && v != last % 32767 + 1) ++out.jumps;
            if (v != 0) last = v;
        }
        if (static_cast<double>(p) * kPollSeconds > 1.0) out.largest_queued = std::max(out.largest_queued, audio_test_queued(kChannel));
    }
    out.queued_at_end = audio_test_queued(kChannel);
    audio_test_reserve(kChannel, 0); // empty again for the next run
    return out;
}

double ms(std::size_t frames) { return static_cast<double>(frames) * 1000.0 / kRate; }

} // namespace

int main() {
    mhf::psp::RuntimeOptions options; // a runtime without a program or a window: nothing is played
    mhf::psp::Runtime rt(options);
    // The voice mixer's channel (256-frame blocks), sound effects: a device 0.1 % slow for three minutes must not leave
    // the effects more than a few tens of milliseconds behind.
    const Run effects = run(rt, 256, 180.0, 0.999);
    std::printf("effects, device 0.1%% slow: %.1f ms queued at the end, %.1f ms at most, %zu skips\n", ms(effects.queued_at_end),
                ms(effects.largest_queued), effects.jumps);
    MHF_CHECK(ms(effects.largest_queued) < 80.0);

    // The music's channel (2048-frame blocks) the same way: a larger cushion, still well below the old 250 ms.
    const Run music = run(rt, 2048, 180.0, 0.999);
    std::printf("music, device 0.1%% slow: %.1f ms queued at the end, %.1f ms at most, %zu skips\n", ms(music.queued_at_end),
                ms(music.largest_queued), music.jumps);
    MHF_CHECK(ms(music.largest_queued) < 200.0);

    // A device at the system clock's rate: nothing is ever dropped, for either kind of channel.
    const Run steady_effects = run(rt, 256, 60.0, 1.0);
    const Run steady_music = run(rt, 2048, 60.0, 1.0);
    std::printf("device at the right rate: %zu skips (effects), %zu skips (music)\n", steady_effects.jumps, steady_music.jumps);
    MHF_CHECK(steady_effects.jumps == 0);
    MHF_CHECK(steady_music.jumps == 0);

    // A device a little fast drains the channel instead: it pauses and refills, never skips forward.
    const Run fast = run(rt, 256, 60.0, 1.001);
    MHF_CHECK(fast.jumps == 0);

    return mhf::test::finish("audio_mix");
}
