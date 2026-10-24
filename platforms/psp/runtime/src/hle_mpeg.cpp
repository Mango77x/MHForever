// sceMpeg: the game's pre-rendered movies (docs/05-runtime-hle.md §5h).
//
// The movies are Sony PSMF files: an MPEG program stream of 2048-byte packs, holding H.264 pictures and ATRAC3+ sound.
// The game reads the file into the ring buffer it created (sceMpegRingbufferPut calls back into the game's own read
// function), and then asks for access units and decodes them. None of the library's demultiplexer or decoders are
// reimplemented byte by byte here: media/psmf_demuxer splits the packs, FFmpeg's libavcodec (third_party, LGPL) decodes
// the pictures and third_party/at3_standalone the sound. See THIRD_PARTY.md.
//
// sceMpegAvcDecode gives the game finished pixels, not YCbCr: with the default decode mode the PSP library converts the
// picture itself, and this game imports neither sceMpegAvcDecodeMode nor sceJpegCsc, so the frames it reads are 32-bit
// RGBA (memory order R, G, B, A) with the row stride it asks for.
//
// The shape of this HLE (the ring buffer fields, the access unit layout, the order of the calls and the pixel layout)
// was cross-checked against PPSSPP's sceMpeg and MediaEngine (GPL-2.0-or-later) and against Yakumo's MHP3rd profile;
// no code was copied from either. Without FFmpeg the stream calls report the movie as finished, exactly as before.

#include "hle.hpp"
#include "kernel.hpp"

#include "media/atrac_decoder.hpp"
#include "media/avc_decoder.hpp"
#include "media/psmf_demuxer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mhf::psp::hle {
namespace {

using media::AccessUnit;
using media::AtracDecoder;
using media::AvcDecoder;
using media::Picture;
using media::PsmfDemuxer;

constexpr std::uint32_t kErrorNoData = 0x80618001u;
constexpr std::uint32_t kErrorInvalidValue = 0x806101FEu;
constexpr std::uint32_t kErrorNoMemory = 0x80610022u;
// Work area the library needs (sceMpegQueryMemSize) and the handle it stores at its start.
constexpr std::uint32_t kMpegMemorySize = 0x10000u;
constexpr std::uint32_t kHandleOffset = 0x30u;
// Ring buffer entries cost a packet of data plus this overhead.
constexpr std::uint32_t kRingbufferPacketOverhead = 104u;
// Sizes the library reports for the sound: the elementary stream buffer it hands out and the output of one frame.
constexpr std::uint32_t kAtracEsSize = 2112u;
constexpr std::uint32_t kAtracOutputSize = 8192u;
// PSMF header fields.
constexpr std::uint32_t kPsmfMagic = 0x464D5350u; // "PSMF"
constexpr std::uint32_t kPsmfStreamOffset = 0x08u;
constexpr std::uint32_t kPsmfStreamSize = 0x0Cu;
// A guest call from inside an HLE function returns here; nothing is ever executed at this address.
constexpr std::uint32_t kReturnAddress = 0x0BAD0000u;

// SceMpegRingbuffer.
namespace ring {
constexpr std::uint32_t kPackets = 0x00u;
constexpr std::uint32_t kRead = 0x04u;    // next packet to demultiplex
constexpr std::uint32_t kWritten = 0x08u; // next packet to fill
constexpr std::uint32_t kFilled = 0x0Cu;  // packets holding data
constexpr std::uint32_t kPacketSize = 0x10u;
constexpr std::uint32_t kData = 0x14u;
constexpr std::uint32_t kCallback = 0x18u;
constexpr std::uint32_t kCallbackArgument = 0x1Cu;
constexpr std::uint32_t kDataEnd = 0x20u;
constexpr std::uint32_t kSemaphore = 0x24u;
constexpr std::uint32_t kMpeg = 0x28u;
} // namespace ring

// SceMpegAu: 64-bit PTS and DTS as high and low words, then the ES buffer and the size of the unit.
namespace au {
constexpr std::uint32_t kPtsHigh = 0x00u;
constexpr std::uint32_t kPtsLow = 0x04u;
constexpr std::uint32_t kDtsHigh = 0x08u;
constexpr std::uint32_t kDtsLow = 0x0Cu;
constexpr std::uint32_t kEsBuffer = 0x10u;
constexpr std::uint32_t kSize = 0x14u;
} // namespace au

enum class StreamType : std::uint32_t { Avc = 0u, Atrac = 1u, Pcm = 2u };

struct MpegState {
    std::uint32_t mpeg{};       // guest address of the SceMpeg variable
    std::uint32_t work{};       // the work area sceMpegCreate was given
    std::uint32_t frame_width{}; // the frame width sceMpegCreate was given; games may pass 0 again when decoding
    std::uint32_t ringbuffer{};
    std::uint32_t stream_packs{}; // packs in the movie, from its header
    std::uint32_t packs_demuxed{};
    bool at_end{};  // the read callback ran out of file
    std::map<std::uint32_t, StreamType> streams; // stream handle -> type
    std::uint32_t next_stream{};
    PsmfDemuxer demuxer;
    AvcDecoder video;
    AtracDecoder audio;
    std::optional<AccessUnit> video_unit; // taken by GetAvcAu, not decoded yet
    std::optional<AccessUnit> audio_unit;
    Picture picture;
    std::uint64_t pictures{};
};

// Everything the library keeps behind the game's handles, per runtime.
struct MpegModule {
    std::map<std::uint32_t, std::unique_ptr<MpegState>> instances;
    std::uint32_t pending_stream_packs{}; // from the last header the game queried
};

std::map<const Runtime *, MpegModule> g_modules;

MpegModule &module(Runtime &rt) { return g_modules[&rt]; }

MpegState *find_mpeg(Runtime &rt, std::uint32_t handle) {
    auto &instances = module(rt).instances;
    const auto found = instances.find(handle);
    return found != instances.end() ? found->second.get() : nullptr;
}

void note_once(const char *what) {
    static std::set<std::string> said;
    if (said.insert(what).second) std::fprintf(stderr, "[hle] %s (simplified)\n", what);
}

std::uint32_t load_be32(Runtime &rt, std::uint32_t address) {
    auto &mem = rt.memory();
    return (static_cast<std::uint32_t>(mem.load8(address)) << 24u) |
           (static_cast<std::uint32_t>(mem.load8(address + 1u)) << 16u) |
           (static_cast<std::uint32_t>(mem.load8(address + 2u)) << 8u) | mem.load8(address + 3u);
}

void store_timestamp(Runtime &rt, std::uint32_t high, std::uint32_t low, std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    rt.memory().store32(high, static_cast<std::uint32_t>(bits >> 32u));
    rt.memory().store32(low, static_cast<std::uint32_t>(bits));
}

void write_unit(Runtime &rt, std::uint32_t address, const AccessUnit &unit) {
    if (address == 0u) return;
    store_timestamp(rt, address + au::kPtsHigh, address + au::kPtsLow, unit.pts);
    store_timestamp(rt, address + au::kDtsHigh, address + au::kDtsLow, unit.dts);
    rt.memory().store32(address + au::kSize, static_cast<std::uint32_t>(unit.data.size()));
}

// Calls the game's own read function: `callback(destination, packets, argument)` returns how many packets it read. The
// call runs on this thread's stack with a copy of its registers, so nothing of the caller is clobbered.
std::int32_t call_read_callback(Runtime &rt, Context &ctx, std::uint32_t callback, std::uint32_t destination,
                                std::uint32_t packets, std::uint32_t argument) {
    Context nested = ctx;
    nested.gpr[4] = destination;
    nested.gpr[5] = packets;
    nested.gpr[6] = argument;
    nested.gpr[31] = kReturnAddress;
    rt.call(nested, callback);
    return static_cast<std::int32_t>(nested.gpr[2]);
}

// Hands the ring's packets to the demultiplexer until an access unit of the wanted kind is queued (or nothing is left).
template <typename Ready> bool demux_until(Runtime &rt, MpegState &state, Ready ready) {
    auto &mem = rt.memory();
    const std::uint32_t buffer = state.ringbuffer;
    if (buffer == 0u) return ready();
    const std::uint32_t packets = mem.load32(buffer + ring::kPackets);
    const std::uint32_t data = mem.load32(buffer + ring::kData);
    std::vector<std::uint8_t> pack(media::kPackSize);
    while (!ready()) {
        const std::uint32_t filled = mem.load32(buffer + ring::kFilled);
        if (filled == 0u || packets == 0u) {
            if (state.at_end || (state.stream_packs != 0u && state.packs_demuxed >= state.stream_packs)) {
                state.demuxer.end_of_stream(); // nothing else is coming: the last picture in the buffer is complete
            }
            return ready();
        }
        const std::uint32_t read = mem.load32(buffer + ring::kRead) % packets;
        std::memcpy(pack.data(), mem.raw_pointer(data + read * static_cast<std::uint32_t>(media::kPackSize),
                                                 media::kPackSize),
                    media::kPackSize);
        state.demuxer.push_pack(pack);
        ++state.packs_demuxed;
        mem.store32(buffer + ring::kRead, (read + 1u) % packets);
        mem.store32(buffer + ring::kFilled, filled - 1u);
        if (state.stream_packs != 0u && state.packs_demuxed >= state.stream_packs) state.demuxer.end_of_stream();
    }
    return true;
}

// The picture as the game wants it: 32-bit RGBA (R, G, B, A in memory), `frame_width` pixels per row, one row per
// picture line. The conversion is BT.601 with the range the stream declares, which is what the PSP's converter does.
void write_pixels(Runtime &rt, std::uint32_t buffer, std::uint32_t frame_width, const Picture &picture) {
    if (buffer == 0u || picture.width == 0u || picture.height == 0u) return;
    if (frame_width == 0u) frame_width = picture.width;
    if (frame_width > 2048u) return;
    const std::uint32_t columns = std::min(frame_width, picture.width);
    const std::uint32_t chroma_width = (picture.width + 1u) / 2u;
    std::vector<std::uint32_t> line(frame_width);
    auto &mem = rt.memory();
    for (std::uint32_t y = 0; y < picture.height; ++y) {
        const std::uint8_t *luma = picture.y.data() + static_cast<std::size_t>(y) * picture.width;
        const std::uint8_t *row_cb = picture.cb.data() + static_cast<std::size_t>(y / 2u) * chroma_width;
        const std::uint8_t *row_cr = picture.cr.data() + static_cast<std::size_t>(y / 2u) * chroma_width;
        for (std::uint32_t x = 0; x < columns; ++x) {
            const int luma_value = picture.full_range ? luma[x] : (luma[x] - 16) * 255 / 219;
            const int cb = row_cb[x / 2u] - 128;
            const int cr = row_cr[x / 2u] - 128;
            const int r = luma_value + ((91881 * cr) >> 16);
            const int g = luma_value - ((22554 * cb + 46802 * cr) >> 16);
            const int b = luma_value + ((116130 * cb) >> 16);
            const auto red = static_cast<std::uint32_t>(std::clamp(r, 0, 255));
            const auto green = static_cast<std::uint32_t>(std::clamp(g, 0, 255));
            const auto blue = static_cast<std::uint32_t>(std::clamp(b, 0, 255));
            // Opaque: the game draws the movie over the black screen it clears, with alpha blending on, so a zero
            // alpha (what PPSSPP writes) would leave the picture invisible here.
            line[x] = red | (green << 8u) | (blue << 16u) | 0xFF000000u;
        }
        mem.copy_in(buffer + y * frame_width * 4u,
                    std::span(reinterpret_cast<const std::uint8_t *>(line.data()), static_cast<std::size_t>(columns) * 4u));
    }
}

// ---------------------------------------------------------------- ring buffer

void MpegRingbufferQueryMemSize(Runtime &, Context &ctx) {
    ret(ctx, arg(ctx, 0) * (static_cast<std::uint32_t>(media::kPackSize) + kRingbufferPacketOverhead));
}

// sceMpegRingbufferConstruct(ring, packets, data, size, callback, argument)
void MpegRingbufferConstruct(Runtime &rt, Context &ctx) {
    auto &mem = rt.memory();
    const std::uint32_t buffer = arg(ctx, 0), packets = arg(ctx, 1), data = arg(ctx, 2);
    mem.store32(buffer + ring::kPackets, packets);
    mem.store32(buffer + ring::kRead, 0u);
    mem.store32(buffer + ring::kWritten, 0u);
    mem.store32(buffer + ring::kFilled, 0u);
    mem.store32(buffer + ring::kPacketSize, static_cast<std::uint32_t>(media::kPackSize));
    mem.store32(buffer + ring::kData, data);
    mem.store32(buffer + ring::kCallback, arg(ctx, 4));
    mem.store32(buffer + ring::kCallbackArgument, arg(ctx, 5));
    mem.store32(buffer + ring::kDataEnd, data + packets * static_cast<std::uint32_t>(media::kPackSize));
    mem.store32(buffer + ring::kSemaphore, 0u);
    mem.store32(buffer + ring::kMpeg, 0u);
    ret(ctx, 0);
}

void MpegRingbufferDestruct(Runtime &, Context &ctx) { ret(ctx, 0); }

void MpegRingbufferAvailableSize(Runtime &rt, Context &ctx) {
    auto &mem = rt.memory();
    const std::uint32_t buffer = arg(ctx, 0);
    const std::uint32_t packets = mem.load32(buffer + ring::kPackets);
    const std::uint32_t filled = mem.load32(buffer + ring::kFilled);
    ret(ctx, packets > filled ? packets - filled : 0u);
}

// sceMpegRingbufferPut(ring, packets, available): fills the ring by calling the game's read function, which may stop
// short at the end of the movie.
void MpegRingbufferPut(Runtime &rt, Context &ctx) {
    auto &mem = rt.memory();
    const std::uint32_t buffer = arg(ctx, 0);
    if (buffer == 0u) {
        ret(ctx, 0);
        return;
    }
    const std::uint32_t packets = mem.load32(buffer + ring::kPackets);
    if (packets == 0u) {
        ret(ctx, 0);
        return;
    }
    std::uint32_t request = std::min(arg(ctx, 1), packets - std::min(mem.load32(buffer + ring::kFilled), packets));
    const std::uint32_t data = mem.load32(buffer + ring::kData);
    const std::uint32_t callback = mem.load32(buffer + ring::kCallback);
    const std::uint32_t argument = mem.load32(buffer + ring::kCallbackArgument);
    std::uint32_t total = 0;
    bool exhausted = false;
    while (request > 0u) {
        const std::uint32_t written = mem.load32(buffer + ring::kWritten) % packets;
        const std::uint32_t chunk = std::min(request, packets - written); // one run up to the end of the ring
        const std::uint32_t target = data + written * static_cast<std::uint32_t>(media::kPackSize);
        const std::int32_t read = call_read_callback(rt, ctx, callback, target, chunk, argument);
        if (read <= 0) {
            exhausted = true;
            break;
        }
        const std::uint32_t count = std::min(static_cast<std::uint32_t>(read), chunk);
        if (count < chunk) exhausted = true;
        mem.store32(buffer + ring::kWritten, (written + count) % packets);
        mem.store32(buffer + ring::kFilled, mem.load32(buffer + ring::kFilled) + count);
        total += count;
        request -= count;
        if (exhausted) break;
    }
    if (exhausted) {
        if (MpegState *state = find_mpeg(rt, mem.load32(buffer + ring::kMpeg))) state->at_end = true;
    }
    ret(ctx, total);
}

// ---------------------------------------------------------------- library and streams

void MpegQueryMemSize(Runtime &, Context &ctx) { ret(ctx, kMpegMemorySize); }

// sceMpegCreate(mpeg, work, size, ring, frameWidth, mode, ddrTop)
void MpegCreate(Runtime &rt, Context &ctx) {
    auto &mem = rt.memory();
    const std::uint32_t mpeg = arg(ctx, 0), work = arg(ctx, 1), ring = arg(ctx, 3);
    if (mpeg == 0u || work == 0u || arg(ctx, 2) < kMpegMemorySize) {
        ret(ctx, kErrorNoMemory);
        return;
    }
    auto state = std::make_unique<MpegState>();
    state->mpeg = mpeg;
    state->work = work;
    state->ringbuffer = ring;
    // sceMpegCreate takes the frame width the game displays pictures at; sceMpegAvcDecode passes it again, but games
    // (MHFU among them) pass 0 there and expect the library to remember this one.
    state->frame_width = arg(ctx, 4);
    state->stream_packs = module(rt).pending_stream_packs;
    mem.store32(mpeg, work + kHandleOffset);
    const char magic[12] = {'L', 'I', 'B', 'M', 'P', 'E', 'G', 0, '0', '0', '1', 0};
    for (std::uint32_t i = 0; i < 12; ++i) mem.store8(work + kHandleOffset + i, static_cast<std::uint8_t>(magic[i]));
    if (ring != 0u) mem.store32(ring + ring::kMpeg, mpeg);
    if (!state->video.open()) note_once("sceMpeg has no H.264 decoder, so movies are skipped");
    module(rt).instances[mpeg] = std::move(state);
    ret(ctx, 0);
}

void MpegDelete(Runtime &rt, Context &ctx) {
    module(rt).instances.erase(arg(ctx, 0));
    ret(ctx, 0);
}

// sceMpegQueryStreamOffset(mpeg, header, outOffset): the PSMF says where its stream starts and how long it is.
void MpegQueryStreamOffset(Runtime &rt, Context &ctx) {
    auto &mem = rt.memory();
    const std::uint32_t header = arg(ctx, 1);
    if (load_be32(rt, header) != kPsmfMagic) {
        mem.store32(arg(ctx, 2), 0u);
        ret(ctx, kErrorInvalidValue);
        return;
    }
    const std::uint32_t offset = load_be32(rt, header + kPsmfStreamOffset);
    const std::uint32_t size = load_be32(rt, header + kPsmfStreamSize);
    module(rt).pending_stream_packs = size / static_cast<std::uint32_t>(media::kPackSize);
    if (MpegState *state = find_mpeg(rt, arg(ctx, 0))) state->stream_packs = module(rt).pending_stream_packs;
    mem.store32(arg(ctx, 2), offset);
    ret(ctx, 0);
}

void MpegQueryStreamSize(Runtime &rt, Context &ctx) {
    const std::uint32_t size = load_be32(rt, arg(ctx, 0) + kPsmfStreamSize);
    if ((size & (static_cast<std::uint32_t>(media::kPackSize) - 1u)) != 0u) {
        ret(ctx, kErrorInvalidValue);
        return;
    }
    rt.memory().store32(arg(ctx, 1), size);
    ret(ctx, 0);
}

// sceMpegRegistStream(mpeg, type, number) -> a handle inside the work area, as the library's own handles are
void MpegRegistStream(Runtime &rt, Context &ctx) {
    MpegState *state = find_mpeg(rt, arg(ctx, 0));
    if (state == nullptr) {
        ret(ctx, 0);
        return;
    }
    const std::uint32_t handle = state->work + 0x100u + 0x10u * state->next_stream++;
    state->streams[handle] = static_cast<StreamType>(arg(ctx, 1));
    ret(ctx, handle);
}

void MpegUnRegistStream(Runtime &rt, Context &ctx) {
    if (MpegState *state = find_mpeg(rt, arg(ctx, 0))) state->streams.erase(arg(ctx, 1));
    ret(ctx, 0);
}

void MpegMallocAvcEsBuf(Runtime &, Context &ctx) { ret(ctx, 1); }
void MpegFreeAvcEsBuf(Runtime &, Context &ctx) { ret(ctx, 0); }

// sceMpegInitAu(mpeg, esBuffer, au)
void MpegInitAu(Runtime &rt, Context &ctx) {
    const std::uint32_t unit = arg(ctx, 2);
    store_timestamp(rt, unit + au::kPtsHigh, unit + au::kPtsLow, -1);
    store_timestamp(rt, unit + au::kDtsHigh, unit + au::kDtsLow, -1);
    rt.memory().store32(unit + au::kEsBuffer, arg(ctx, 1));
    rt.memory().store32(unit + au::kSize, 0u);
    ret(ctx, 0);
}

void MpegQueryAtracEsSize(Runtime &rt, Context &ctx) {
    rt.memory().store32(arg(ctx, 1), kAtracEsSize);
    rt.memory().store32(arg(ctx, 2), kAtracOutputSize);
    ret(ctx, 0);
}

void MpegFlushAllStream(Runtime &rt, Context &ctx) {
    if (MpegState *state = find_mpeg(rt, arg(ctx, 0))) {
        state->demuxer.reset();
        state->video_unit.reset();
        state->audio_unit.reset();
        state->packs_demuxed = 0;
        state->at_end = false;
    }
    ret(ctx, 0);
}

// sceMpegGetAvcAu(mpeg, stream, au, outAttribute)
void MpegGetAvcAu(Runtime &rt, Context &ctx) {
    MpegState *state = find_mpeg(rt, arg(ctx, 0));
    if (state == nullptr) {
        ret(ctx, kErrorInvalidValue);
        return;
    }
    if (!AvcDecoder::available()) {
        ret(ctx, kErrorNoData); // built without FFmpeg: the movie is over before it starts
        return;
    }
    if (!demux_until(rt, *state, [state] { return state->demuxer.video_ready(); })) {
        ret(ctx, kErrorNoData);
        return;
    }
    state->video_unit = state->demuxer.pop_video();
    if (!state->video_unit) {
        ret(ctx, kErrorNoData);
        return;
    }
    write_unit(rt, arg(ctx, 2), *state->video_unit);
    if (arg(ctx, 3) != 0u) rt.memory().store32(arg(ctx, 3), 1u);
    ret(ctx, 0);
}

// sceMpegGetAtracAu(mpeg, stream, au, outAttribute)
void MpegGetAtracAu(Runtime &rt, Context &ctx) {
    MpegState *state = find_mpeg(rt, arg(ctx, 0));
    if (state == nullptr) {
        ret(ctx, kErrorInvalidValue);
        return;
    }
    if (!demux_until(rt, *state, [state] { return state->demuxer.audio_ready(); })) {
        ret(ctx, kErrorNoData);
        return;
    }
    state->audio_unit = state->demuxer.pop_audio();
    if (!state->audio_unit) {
        ret(ctx, kErrorNoData);
        return;
    }
    write_unit(rt, arg(ctx, 2), *state->audio_unit);
    if (arg(ctx, 3) != 0u) rt.memory().store32(arg(ctx, 3), 0u);
    ret(ctx, 0);
}

// ---------------------------------------------------------------- decoding

// Feeds access units to the decoder until a picture comes out: with B-frames the decoder holds each picture back until
// it has seen the next one, and the guest reads exactly one frame per call.
bool decode_next_picture(Runtime &rt, MpegState &state) {
    for (;;) {
        if (state.video_unit) {
            const auto unit = std::move(*state.video_unit);
            state.video_unit.reset();
            if (state.video.decode(unit.data, state.picture)) return true;
            continue;
        }
        if (!demux_until(rt, state, [&state] { return state.demuxer.video_ready(); })) break;
        const auto unit = state.demuxer.pop_video();
        if (!unit) break;
        if (state.video.decode(unit->data, state.picture)) return true;
    }
    // The movie is over: whatever the decoder still holds is the last picture or two.
    return state.video.drain(state.picture);
}

// sceMpegAvcDecode(mpeg, au, frameWidth, outBufferPointer, outInit)
void MpegAvcDecode(Runtime &rt, Context &ctx) {
    MpegState *state = find_mpeg(rt, arg(ctx, 0));
    if (state == nullptr) {
        ret(ctx, kErrorInvalidValue);
        return;
    }
    auto &mem = rt.memory();
    // MHFU passes 0 here and relies on the frame width the library kept from sceMpegCreate (sceMpegCreate stores it;
    // PPSSPP behaves the same through its "defaultFrameWidth").
    std::uint32_t frame_width = arg(ctx, 2) != 0u ? arg(ctx, 2) : state->frame_width;
    const std::uint32_t buffer = arg(ctx, 3) != 0u ? mem.load32(arg(ctx, 3)) : 0u;
    const bool produced = decode_next_picture(rt, *state);
    if (produced) {
        ++state->pictures;
        write_pixels(rt, buffer, frame_width, state->picture);
    }
    if (arg(ctx, 4) != 0u) mem.store32(arg(ctx, 4), produced ? 1u : 0u);
    ret(ctx, 0);
}

// sceMpegAvcDecodeStop(mpeg, frameWidth, outBufferPointer, outStatus): the pictures the decoder still holds.
void MpegAvcDecodeStop(Runtime &rt, Context &ctx) {
    MpegState *state = find_mpeg(rt, arg(ctx, 0));
    auto &mem = rt.memory();
    const bool produced = state != nullptr && state->video.drain(state->picture);
    const std::uint32_t frame_width =
        (state != nullptr && arg(ctx, 1) != 0u) ? arg(ctx, 1) : (state != nullptr ? state->frame_width : 0u);
    if (produced && arg(ctx, 2) != 0u) {
        const std::uint32_t buffer = mem.load32(arg(ctx, 2));
        write_pixels(rt, buffer, frame_width, state->picture);
    }
    if (arg(ctx, 3) != 0u) mem.store32(arg(ctx, 3), produced ? 1u : 0u);
    ret(ctx, 0);
}

// sceMpegAtracDecode(mpeg, au, outBuffer, outInit): one ATRAC3+ frame as interleaved 16-bit samples.
void MpegAtracDecode(Runtime &rt, Context &ctx) {
    MpegState *state = find_mpeg(rt, arg(ctx, 0));
    if (state == nullptr) {
        ret(ctx, kErrorInvalidValue);
        return;
    }
    auto &mem = rt.memory();
    const unsigned channels = std::max(1u, state->demuxer.audio_channels());
    if (!state->audio.is_open() && state->demuxer.audio_frame_size() > media::kAtracFrameHeader) {
        state->audio.open(channels, static_cast<unsigned>(state->demuxer.audio_frame_size() - media::kAtracFrameHeader));
    }
    std::vector<std::int16_t> samples(static_cast<std::size_t>(media::kAtracSamplesPerFrame) * 2u, 0);
    if (state->audio_unit) {
        if (state->audio.is_open()) state->audio.decode(state->audio_unit->data, samples.data());
        state->audio_unit.reset();
    }
    if (arg(ctx, 2) != 0u) {
        const auto bytes = std::span(reinterpret_cast<const std::uint8_t *>(samples.data()), samples.size() * 2u);
        mem.copy_in(arg(ctx, 2), bytes);
    }
    // The fourth argument is a flag, not an output address (the library always fills the buffer, with silence when
    // there is nothing to decode).
    ret(ctx, 0);
}

void MpegZero(Runtime &, Context &ctx) { ret(ctx, 0); }

const HleFunction kFunctions[] = {
    {"sceMpeg", 0xC132E22Fu, "sceMpegQueryMemSize", &MpegQueryMemSize},
    {"sceMpeg", 0xD7A29F46u, "sceMpegRingbufferQueryMemSize", &MpegRingbufferQueryMemSize},
    {"sceMpeg", 0x37295ED8u, "sceMpegRingbufferConstruct", &MpegRingbufferConstruct},
    {"sceMpeg", 0x13407F13u, "sceMpegRingbufferDestruct", &MpegRingbufferDestruct},
    {"sceMpeg", 0xB5F6DC87u, "sceMpegRingbufferAvailableSize", &MpegRingbufferAvailableSize},
    {"sceMpeg", 0xB240A59Eu, "sceMpegRingbufferPut", &MpegRingbufferPut},
    {"sceMpeg", 0x682A619Bu, "sceMpegInit", &MpegZero},
    {"sceMpeg", 0x874624D6u, "sceMpegFinish", &MpegZero},
    {"sceMpeg", 0xD8C5F121u, "sceMpegCreate", &MpegCreate},
    {"sceMpeg", 0x606A4649u, "sceMpegDelete", &MpegDelete},
    {"sceMpeg", 0x21FF80E4u, "sceMpegQueryStreamOffset", &MpegQueryStreamOffset},
    {"sceMpeg", 0x611E9E11u, "sceMpegQueryStreamSize", &MpegQueryStreamSize},
    {"sceMpeg", 0x42560F23u, "sceMpegRegistStream", &MpegRegistStream},
    {"sceMpeg", 0x591A4AA2u, "sceMpegUnRegistStream", &MpegUnRegistStream},
    {"sceMpeg", 0xA780CF7Eu, "sceMpegMallocAvcEsBuf", &MpegMallocAvcEsBuf},
    {"sceMpeg", 0xCEB870B1u, "sceMpegFreeAvcEsBuf", &MpegFreeAvcEsBuf},
    {"sceMpeg", 0x167AFD9Eu, "sceMpegInitAu", &MpegInitAu},
    {"sceMpeg", 0xF8DCB679u, "sceMpegQueryAtracEsSize", &MpegQueryAtracEsSize},
    {"sceMpeg", 0x707B7629u, "sceMpegFlushAllStream", &MpegFlushAllStream},
    {"sceMpeg", 0xFE246728u, "sceMpegGetAvcAu", &MpegGetAvcAu},
    {"sceMpeg", 0xE1CE83A7u, "sceMpegGetAtracAu", &MpegGetAtracAu},
    {"sceMpeg", 0x0E3C2E9Du, "sceMpegAvcDecode", &MpegAvcDecode},
    {"sceMpeg", 0x740FCCD1u, "sceMpegAvcDecodeStop", &MpegAvcDecodeStop},
    {"sceMpeg", 0x800C44DFu, "sceMpegAtracDecode", &MpegAtracDecode},
};

} // namespace

std::span<const HleFunction> mpeg_functions() { return kFunctions; }

void mpeg_release(Runtime &rt) { g_modules.erase(&rt); }

} // namespace mhf::psp::hle
