// sceAudio, sceSasCore, sceAtrac3plus, sceMpeg and sceUtility dialogs.
//
// First pass: these keep the game's timing and state machines moving (audio output paces itself in real time,
// decoders produce silence, movies report the end of their stream, dialogs finish at once). Real sound and video come next (docs/10-roadmap.md phases 2-3); every simplification is logged once. Savedata requests are carried
// out for real by hle_savedata.cpp.

#include "hle.hpp"
#include "kernel.hpp"

#include "mhf/psp/host.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace mhf::psp::hle {

namespace {

Kernel &k(Runtime &rt) { return rt.kernel(); }

void note_once(const char *what) {
    static std::set<std::string> said;
    if (said.insert(what).second) {
        std::fprintf(stderr, "[hle] %s (simplified)\n", what);
    }
}

// ---------------------------------------------------------------- sceAudio

constexpr int kChannels = 8;
constexpr std::uint32_t kSampleRate = 44100;

struct AudioChannel {
    bool reserved{};
    std::uint32_t samples{};
    std::uint32_t format{}; // 0 stereo, 0x10 mono
    std::uint64_t busy_until{};
    std::deque<std::int16_t> queue; // interleaved stereo waiting for the host mixer
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
    // The hardware buffers one block: block only while the previous one is still playing.
    if (c.busy_until > now) {
        k(rt).wait(Kernel::WaitType::Delay, 0, c.busy_until);
    }
    const std::uint64_t start = std::max(c.busy_until, k(rt).now_us());
    c.busy_until = start + static_cast<std::uint64_t>(c.samples) * 1'000'000u / kSampleRate;
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

// ---------------------------------------------------------------- sceSasCore (silent mixer)

std::uint32_t g_sas_grain = 256;

void SasInit(Runtime &, Context &ctx) {
    note_once("sceSasCore mixes silence");
    g_sas_grain = arg(ctx, 1) != 0 ? arg(ctx, 1) : 256;
    ret(ctx, 0);
}

void SasCore(Runtime &rt, Context &ctx) {
    rt.memory().zero(arg(ctx, 1), g_sas_grain * 4u); // stereo 16-bit
    ret(ctx, 0);
}

void SasGetEndFlag(Runtime &, Context &ctx) { ret(ctx, 0xFFFFFFFFu); } // every voice has ended
void SasGetOutputmode(Runtime &, Context &ctx) { ret(ctx, 0); }
void SasZero(Runtime &, Context &ctx) { ret(ctx, 0); }

// ---------------------------------------------------------------- sceAtrac3plus (silent decoder)

int g_next_atrac = 0;

void AtracSetDataAndGetID(Runtime &, Context &ctx) {
    note_once("sceAtrac3plus decodes silence");
    ret(ctx, static_cast<std::uint32_t>(g_next_atrac++ % 6));
}

// (id, out, *samples, *end, *remainFrame)
void AtracDecodeData(Runtime &rt, Context &ctx) {
    constexpr std::uint32_t kFrameSamples = 2048;
    auto &mem = rt.memory();
    if (arg(ctx, 1) != 0) mem.zero(arg(ctx, 1), kFrameSamples * 4u);
    if (arg(ctx, 2) != 0) mem.store32(arg(ctx, 2), kFrameSamples);
    if (arg(ctx, 3) != 0) mem.store32(arg(ctx, 3), 0);
    if (arg(ctx, 4) != 0) mem.store32(arg(ctx, 4), 0xFFFFFFFFu); // all data is in memory
    ret(ctx, 0);
}

void AtracGetRemainFrame(Runtime &rt, Context &ctx) {
    if (arg(ctx, 1) != 0) rt.memory().store32(arg(ctx, 1), 0xFFFFFFFFu);
    ret(ctx, 0);
}

void AtracGetSoundSample(Runtime &rt, Context &ctx) {
    if (arg(ctx, 1) != 0) rt.memory().store32(arg(ctx, 1), 0x100000u); // end sample
    if (arg(ctx, 2) != 0) rt.memory().store32(arg(ctx, 2), 0xFFFFFFFFu); // loop start: none
    if (arg(ctx, 3) != 0) rt.memory().store32(arg(ctx, 3), 0xFFFFFFFFu);
    ret(ctx, 0);
}

void AtracGetBitrate(Runtime &rt, Context &ctx) {
    if (arg(ctx, 1) != 0) rt.memory().store32(arg(ctx, 1), 64);
    ret(ctx, 0);
}

void AtracGetLoopStatus(Runtime &rt, Context &ctx) {
    if (arg(ctx, 1) != 0) rt.memory().store32(arg(ctx, 1), 0);
    if (arg(ctx, 2) != 0) rt.memory().store32(arg(ctx, 2), 0);
    ret(ctx, 0);
}

void AtracGetNextDecodePosition(Runtime &rt, Context &ctx) {
    if (arg(ctx, 1) != 0) rt.memory().store32(arg(ctx, 1), 0);
    ret(ctx, 0);
}

void AtracGetStreamDataInfo(Runtime &rt, Context &ctx) {
    for (unsigned i = 1; i <= 3; ++i) {
        if (arg(ctx, i) != 0) rt.memory().store32(arg(ctx, i), 0);
    }
    ret(ctx, 0);
}

void AtracZero(Runtime &, Context &ctx) { ret(ctx, 0); }

// ---------------------------------------------------------------- sceMpeg (movies end immediately)

constexpr std::uint32_t kMpegNoData = 0x80618001u;

void MpegQueryMemSize(Runtime &rt, Context &ctx) {
    note_once("sceMpeg plays no video");
    (void)rt;
    ret(ctx, 0x10000u); // sceMpegQueryMemSize(mode) returns the work area size itself
}

void MpegRingbufferQueryMemSize(Runtime &, Context &ctx) { ret(ctx, arg(ctx, 0) * 2048u); }

void MpegRingbufferConstruct(Runtime &rt, Context &ctx) {
    // SceMpegRingbuffer: packets, packetsRead, packetsWritten, packetsAvail, packetSize, data, callback, cbArg,
    // dataUpperBound, semaId, mpeg.
    auto &mem = rt.memory();
    const std::uint32_t rb = arg(ctx, 0), packets = arg(ctx, 1), data = arg(ctx, 2);
    mem.store32(rb + 0, packets);
    mem.store32(rb + 4, 0);
    mem.store32(rb + 8, 0);
    mem.store32(rb + 12, 0);
    mem.store32(rb + 16, 2048);
    mem.store32(rb + 20, data);
    mem.store32(rb + 24, arg(ctx, 4));
    mem.store32(rb + 28, arg(ctx, 5));
    mem.store32(rb + 32, data + packets * 2048u);
    mem.store32(rb + 36, 0);
    mem.store32(rb + 40, 0);
    ret(ctx, 0);
}

// sceMpegCreate(mpeg, data, size, ringbuffer, frameWidth, mode, ddrTop): the handle points into the work area, which
// starts with the library's "LIBMPEG" header; the ring buffer records its owner.
void MpegCreate(Runtime &rt, Context &ctx) {
    auto &mem = rt.memory();
    const std::uint32_t mpeg = arg(ctx, 0), data = arg(ctx, 1), ring = arg(ctx, 3);
    if (mpeg == 0 || data == 0 || arg(ctx, 2) < 0x10000u) {
        ret(ctx, 0x80618006u); // SCE_MPEG_ERROR_NO_MEMORY
        return;
    }
    const std::uint32_t handle = data + 0x30u;
    mem.store32(mpeg, handle);
    const char magic[12] = {'L', 'I', 'B', 'M', 'P', 'E', 'G', 0, '0', '0', '1', 0};
    for (std::uint32_t i = 0; i < 12; ++i) mem.store8(handle + i, static_cast<std::uint8_t>(magic[i]));
    mem.store32(handle + 12, 0xFFFFFFFFu);
    mem.store32(handle + 16, ring);
    mem.store32(handle + 20, ring != 0 ? mem.load32(ring + 32) : 0u);
    if (ring != 0) mem.store32(ring + 40, mpeg);
    ret(ctx, 0);
}

void MpegRingbufferAvailableSize(Runtime &rt, Context &ctx) {
    ret(ctx, rt.memory().load32(arg(ctx, 0)) - rt.memory().load32(arg(ctx, 0) + 12)); // free packets
}
void MpegQueryStreamOffset(Runtime &rt, Context &ctx) {
    if (arg(ctx, 2) != 0) rt.memory().store32(arg(ctx, 2), 2048);
    ret(ctx, 0);
}
void MpegQueryStreamSize(Runtime &rt, Context &ctx) {
    if (arg(ctx, 1) != 0) rt.memory().store32(arg(ctx, 1), 0);
    ret(ctx, 0);
}
void MpegRegistStream(Runtime &, Context &ctx) { ret(ctx, 0x34340000u); }
void MpegMallocAvcEsBuf(Runtime &, Context &ctx) { ret(ctx, 1); }
void MpegGetAu(Runtime &, Context &ctx) { ret(ctx, kMpegNoData); }
void MpegAvcDecode(Runtime &rt, Context &ctx) {
    if (arg(ctx, 4) != 0) rt.memory().store32(arg(ctx, 4), 0); // no frame produced
    ret(ctx, 0);
}
void MpegZero(Runtime &, Context &ctx) { ret(ctx, 0); }

// ---------------------------------------------------------------- sceUtility dialogs

enum DialogStatus : std::uint32_t { kNone = 0, kInit = 1, kRunning = 2, kFinished = 3, kShutdown = 4 };

struct Dialog {
    std::uint32_t status{kNone};
    std::uint32_t params{};
};
Dialog g_savedata, g_message, g_osk, g_netconf;

std::uint32_t get_status(Dialog &d) {
    const std::uint32_t s = d.status;
    if (s == kShutdown) d.status = kNone;
    if (s == kInit) d.status = kRunning;
    return s;
}

void SavedataInitStart(Runtime &rt, Context &ctx) {
    if (g_savedata.status != kNone) {
        ret(ctx, 0x80110005u); // SCE_ERROR_UTILITY_WRONG_TYPE / busy
        return;
    }
    g_savedata.params = arg(ctx, 0);
    execute_savedata(rt, g_savedata.params); // the result goes into the parameter block
    g_savedata.status = kInit;
    ret(ctx, 0);
}

void SavedataUpdate(Runtime &, Context &ctx) {
    if (g_savedata.status == kRunning || g_savedata.status == kInit) g_savedata.status = kFinished;
    ret(ctx, 0);
}
void SavedataGetStatus(Runtime &, Context &ctx) { ret(ctx, get_status(g_savedata)); }
void SavedataShutdownStart(Runtime &, Context &ctx) {
    g_savedata.status = kShutdown;
    ret(ctx, 0);
}

void MsgDialogInitStart(Runtime &rt, Context &ctx) {
    g_message.params = arg(ctx, 0);
    std::fprintf(stderr, "[hle] message dialog (mode %u, options %X, error %08X): \"%s\"\n",
                 rt.memory().load32(g_message.params + 0x34), rt.memory().load32(g_message.params + 0x23C),
                 rt.memory().load32(g_message.params + 0x38), read_string(rt, g_message.params + 0x3C, 512).c_str());
    rt.memory().store32(g_message.params + 0x1C, 0);     // result
    // No dialog UI yet: answer as a player who accepts. Yes/no questions get "yes" (1); messages with only an OK
    // button get 1 too; plain messages, whose only button is "back", get "back" (3).
    const std::uint32_t options = rt.memory().load32(g_message.params + 0x23C);
    const bool has_buttons = (options & 0x10u) != 0 || (options & 0x20u) != 0; // YESNO, OK
    rt.memory().store32(g_message.params + 0x240, has_buttons ? 1u : 3u);
    g_message.status = kInit;
    ret(ctx, 0);
}
void MsgDialogUpdate(Runtime &, Context &ctx) {
    if (g_message.status == kRunning || g_message.status == kInit) g_message.status = kFinished;
    ret(ctx, 0);
}
void MsgDialogGetStatus(Runtime &, Context &ctx) { ret(ctx, get_status(g_message)); }
void MsgDialogShutdownStart(Runtime &, Context &ctx) {
    g_message.status = kShutdown;
    ret(ctx, 0);
}

// UTF-16 string from guest memory (up to `max` units), as UTF-8 for logs (non-ASCII shown as '?').
std::string read_utf16(Runtime &rt, std::uint32_t address, std::uint32_t max) {
    std::string out;
    if (address == 0) return out;
    for (std::uint32_t i = 0; i < max; ++i) {
        const std::uint16_t c = rt.memory().load16(address + i * 2u);
        if (c == 0) break;
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

// No keyboard UI yet: every field the game asks for gets its initial text, or RuntimeOptions::osk_default_text when
// that is empty, as if the player had typed it and pressed Enter.
void OskInitStart(Runtime &rt, Context &ctx) {
    g_osk.params = arg(ctx, 0);
    auto &mem = rt.memory();
    const std::uint32_t count = mem.load32(g_osk.params + 0x30), fields = mem.load32(g_osk.params + 0x34);
    for (std::uint32_t i = 0; fields != 0 && i < count && i < 16; ++i) {
        const std::uint32_t field = fields + i * 0x34u;
        const std::uint32_t intext = mem.load32(field + 32), capacity = mem.load32(field + 36);
        const std::uint32_t outtext = mem.load32(field + 40), limit = mem.load32(field + 48);
        std::u16string text;
        for (std::uint32_t j = 0; intext != 0 && j < 512; ++j) {
            const std::uint16_t c = mem.load16(intext + j * 2u);
            if (c == 0) break;
            text.push_back(static_cast<char16_t>(c));
        }
        if (text.empty()) {
            for (const char c : rt.options().osk_default_text) text.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
        }
        std::size_t max = text.size();
        if (limit != 0) max = std::min<std::size_t>(max, limit);
        if (capacity != 0) max = std::min<std::size_t>(max, capacity - 1u);
        if (outtext != 0) {
            for (std::size_t j = 0; j < max; ++j) mem.store16(outtext + static_cast<std::uint32_t>(j) * 2u, text[j]);
            mem.store16(outtext + static_cast<std::uint32_t>(max) * 2u, 0);
        }
        mem.store32(field + 44, 2); // PSP_UTILITY_OSK_RESULT_CHANGED
        std::fprintf(stderr, "[hle] on-screen keyboard \"%s\" -> \"%s\"\n",
                     read_utf16(rt, mem.load32(field + 28), 64).c_str(), read_utf16(rt, outtext, 64).c_str());
    }
    mem.store32(g_osk.params + 0x1C, 0);
    g_osk.status = kInit;
    ret(ctx, 0);
}
void OskUpdate(Runtime &, Context &ctx) {
    if (g_osk.status == kRunning || g_osk.status == kInit) g_osk.status = kFinished;
    ret(ctx, 0);
}
void OskGetStatus(Runtime &, Context &ctx) { ret(ctx, get_status(g_osk)); }
void OskShutdownStart(Runtime &, Context &ctx) {
    g_osk.status = kShutdown;
    ret(ctx, 0);
}

void NetconfInitStart(Runtime &rt, Context &ctx) {
    g_netconf.params = arg(ctx, 0);
    rt.memory().store32(g_netconf.params + 0x1C, 0);
    g_netconf.status = kInit;
    ret(ctx, 0);
}
void NetconfUpdate(Runtime &, Context &ctx) {
    if (g_netconf.status == kRunning || g_netconf.status == kInit) g_netconf.status = kFinished;
    ret(ctx, 0);
}
void NetconfGetStatus(Runtime &, Context &ctx) { ret(ctx, get_status(g_netconf)); }
void NetconfShutdownStart(Runtime &, Context &ctx) {
    g_netconf.status = kShutdown;
    ret(ctx, 0);
}

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
    {"sceSasCore", 0x42778A9Fu, "__sceSasInit", &SasInit},
    {"sceSasCore", 0xA3589D81u, "__sceSasCore", &SasCore},
    {"sceSasCore", 0x68A46B95u, "__sceSasGetEndFlag", &SasGetEndFlag},
    {"sceSasCore", 0xE175EF66u, "__sceSasGetOutputmode", &SasGetOutputmode},
    {"sceSasCore", 0x267A6DD2u, "__sceSasRevParam", &SasZero},
    {"sceSasCore", 0x33D4AB37u, "__sceSasRevType", &SasZero},
    {"sceSasCore", 0x440CA7D8u, "__sceSasSetVolume", &SasZero},
    {"sceSasCore", 0x76F01ACAu, "__sceSasSetKeyOn", &SasZero},
    {"sceSasCore", 0x99944089u, "__sceSasSetVoice", &SasZero},
    {"sceSasCore", 0xA0CF2FA4u, "__sceSasSetKeyOff", &SasZero},
    {"sceSasCore", 0xAD84D37Fu, "__sceSasSetPitch", &SasZero},
    {"sceSasCore", 0xCBCD4F79u, "__sceSasSetSimpleADSR", &SasZero},
    {"sceSasCore", 0xD5A229C9u, "__sceSasRevEVOL", &SasZero},
    {"sceSasCore", 0xF983B186u, "__sceSasRevVON", &SasZero},
    {"sceAtrac3plus", 0x7A20E7AFu, "sceAtracSetDataAndGetID", &AtracSetDataAndGetID},
    {"sceAtrac3plus", 0x6A8C3CD5u, "sceAtracDecodeData", &AtracDecodeData},
    {"sceAtrac3plus", 0x9AE849A7u, "sceAtracGetRemainFrame", &AtracGetRemainFrame},
    {"sceAtrac3plus", 0xA2BBA8BEu, "sceAtracGetSoundSample", &AtracGetSoundSample},
    {"sceAtrac3plus", 0xA554A158u, "sceAtracGetBitrate", &AtracGetBitrate},
    {"sceAtrac3plus", 0xFAA4F89Bu, "sceAtracGetLoopStatus", &AtracGetLoopStatus},
    {"sceAtrac3plus", 0xE23E3A35u, "sceAtracGetNextDecodePosition", &AtracGetNextDecodePosition},
    {"sceAtrac3plus", 0x5D268707u, "sceAtracGetStreamDataInfo", &AtracGetStreamDataInfo},
    {"sceAtrac3plus", 0x61EB33F5u, "sceAtracReleaseAtracID", &AtracZero},
    {"sceAtrac3plus", 0x868120B5u, "sceAtracSetLoopNum", &AtracZero},
    {"sceMpeg", 0xC132E22Fu, "sceMpegQueryMemSize", &MpegQueryMemSize},
    {"sceMpeg", 0xD7A29F46u, "sceMpegRingbufferQueryMemSize", &MpegRingbufferQueryMemSize},
    {"sceMpeg", 0x37295ED8u, "sceMpegRingbufferConstruct", &MpegRingbufferConstruct},
    {"sceMpeg", 0xB5F6DC87u, "sceMpegRingbufferAvailableSize", &MpegRingbufferAvailableSize},
    {"sceMpeg", 0x21FF80E4u, "sceMpegQueryStreamOffset", &MpegQueryStreamOffset},
    {"sceMpeg", 0x611E9E11u, "sceMpegQueryStreamSize", &MpegQueryStreamSize},
    {"sceMpeg", 0x42560F23u, "sceMpegRegistStream", &MpegRegistStream},
    {"sceMpeg", 0xA780CF7Eu, "sceMpegMallocAvcEsBuf", &MpegMallocAvcEsBuf},
    {"sceMpeg", 0xFE246728u, "sceMpegGetAvcAu", &MpegGetAu},
    {"sceMpeg", 0xE1CE83A7u, "sceMpegGetAtracAu", &MpegGetAu},
    {"sceMpeg", 0x0E3C2E9Du, "sceMpegAvcDecode", &MpegAvcDecode},
    {"sceMpeg", 0x682A619Bu, "sceMpegInit", &MpegZero},
    {"sceMpeg", 0x874624D6u, "sceMpegFinish", &MpegZero},
    {"sceMpeg", 0xD8C5F121u, "sceMpegCreate", &MpegCreate},
    {"sceMpeg", 0x606A4649u, "sceMpegDelete", &MpegZero},
    {"sceMpeg", 0x13407F13u, "sceMpegRingbufferDestruct", &MpegZero},
    {"sceMpeg", 0xB240A59Eu, "sceMpegRingbufferPut", &MpegZero},
    {"sceMpeg", 0x167AFD9Eu, "sceMpegInitAu", &MpegZero},
    {"sceMpeg", 0x591A4AA2u, "sceMpegUnRegistStream", &MpegZero},
    {"sceMpeg", 0x707B7629u, "sceMpegFlushAllStream", &MpegZero},
    {"sceMpeg", 0x740FCCD1u, "sceMpegAvcDecodeStop", &MpegZero},
    {"sceMpeg", 0x800C44DFu, "sceMpegAtracDecode", &MpegZero},
    {"sceMpeg", 0xCEB870B1u, "sceMpegFreeAvcEsBuf", &MpegZero},
    {"sceMpeg", 0xF8DCB679u, "sceMpegQueryAtracEsSize", &MpegZero},
    {"sceUtility", 0x50C4CD57u, "sceUtilitySavedataInitStart", &SavedataInitStart},
    {"sceUtility", 0xD4B95FFBu, "sceUtilitySavedataUpdate", &SavedataUpdate},
    {"sceUtility", 0x8874DBE0u, "sceUtilitySavedataGetStatus", &SavedataGetStatus},
    {"sceUtility", 0x9790B33Cu, "sceUtilitySavedataShutdownStart", &SavedataShutdownStart},
    {"sceUtility", 0x2AD8E239u, "sceUtilityMsgDialogInitStart", &MsgDialogInitStart},
    {"sceUtility", 0x95FC253Bu, "sceUtilityMsgDialogUpdate", &MsgDialogUpdate},
    {"sceUtility", 0x9A1C91D7u, "sceUtilityMsgDialogGetStatus", &MsgDialogGetStatus},
    {"sceUtility", 0x67AF3428u, "sceUtilityMsgDialogShutdownStart", &MsgDialogShutdownStart},
    {"sceUtility", 0xF6269B82u, "sceUtilityOskInitStart", &OskInitStart},
    {"sceUtility", 0x4B85C861u, "sceUtilityOskUpdate", &OskUpdate},
    {"sceUtility", 0xF3F76017u, "sceUtilityOskGetStatus", &OskGetStatus},
    {"sceUtility", 0x3DFAEBA9u, "sceUtilityOskShutdownStart", &OskShutdownStart},
    {"sceUtility", 0x4DB1E739u, "sceUtilityNetconfInitStart", &NetconfInitStart},
    {"sceUtility", 0x91E70E35u, "sceUtilityNetconfUpdate", &NetconfUpdate},
    {"sceUtility", 0x6332AA39u, "sceUtilityNetconfGetStatus", &NetconfGetStatus},
    {"sceUtility", 0xF88155F6u, "sceUtilityNetconfShutdownStart", &NetconfShutdownStart},
};

} // namespace

std::span<const HleFunction> media_functions() { return kFunctions; }

} // namespace mhf::psp::hle

namespace mhf::psp {

std::size_t mix_audio(Runtime &, std::int16_t *out, std::size_t frames) {
    std::size_t available = 0;
    for (std::size_t i = 0; i < frames * 2; ++i) {
        std::int32_t sum = 0;
        for (auto &c : hle::g_channels) {
            if (!c.queue.empty()) {
                sum += c.queue.front();
                c.queue.pop_front();
                available = std::max(available, i / 2 + 1);
            }
        }
        out[i] = static_cast<std::int16_t>(std::clamp(sum, -32768, 32767));
    }
    return available;
}

} // namespace mhf::psp
