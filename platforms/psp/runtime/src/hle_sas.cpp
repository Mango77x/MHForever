// sceSasCore: the PSP's software voice mixer (docs/05-runtime-hle.md §7).
//
// Each call to __sceSasCore mixes one "grain" (usually 256 stereo frames at 44.1 kHz) of every playing voice into a
// guest buffer, which the game then sends to sceAudio. A voice plays PSP ADPCM ("VAG") data at a pitch (0x1000 =
// 44.1 kHz), shaped by an SPU-style ADSR envelope and scaled by per-channel volumes (0x1000 = unity).
//
// Behaviour follows the documented SPU/SAS semantics (envelope rate encoding, VAG block flags); rates and edge cases
// were cross-checked against PPSSPP's implementation (GPL-2.0-or-later). Not implemented yet: reverb (the effect
// send is dropped), PCM and noise voices, ATRAC voices.

#include "hle.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace mhf::psp::hle {

namespace {

constexpr int kMaxVoices = 32;
constexpr std::int64_t kEnvelopeMax = 0x40000000;

constexpr std::uint32_t kErrorInvalidVoice = 0x80420010u;
constexpr std::uint32_t kErrorInvalidSize = 0x8042001Au;
constexpr std::uint32_t kErrorInvalidPitch = 0x80420011u;
constexpr std::uint32_t kErrorInvalidVolume = 0x80420018u;

// ------------------------------------------------------------------ VAG (PSP ADPCM) decoding

class VagDecoder {
public:
    void start(std::uint32_t address, std::uint32_t size, bool loop) {
        data_ = address;
        blocks_ = size / 16u;
        loop_ = loop;
        restart();
    }
    void restart() {
        block_ = 0;
        loop_start_ = 0;
        index_ = 28;
        s1_ = s2_ = 0;
        end_ = blocks_ == 0;
        loop_next_ = false;
    }
    bool ended() const { return end_; }

    std::int16_t next(psprecomp::GuestMemory &mem) {
        if (index_ >= 28) {
            decode_block(mem);
            if (end_) return 0;
        }
        return samples_[index_++];
    }

private:
    void decode_block(psprecomp::GuestMemory &mem) {
        if (loop_next_) {
            loop_next_ = false;
            block_ = loop_start_;
        }
        if (block_ >= blocks_) {
            end_ = true;
            return;
        }
        const std::uint32_t at = data_ + block_ * 16u;
        const std::uint8_t header = mem.load8(at);
        const std::uint8_t flags = mem.load8(at + 1u);
        if (flags == 7) { // end of data, this block is not played
            end_ = true;
            return;
        }
        if (flags == 6) loop_start_ = block_;
        if (flags == 3 && loop_) loop_next_ = true;
        static constexpr int kCoefficients[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
        const int shift = header & 0x0F;
        const int predictor = std::min(header >> 4, 4);
        const int c0 = kCoefficients[predictor][0], c1 = kCoefficients[predictor][1];
        for (int i = 0; i < 14; ++i) {
            const std::uint8_t byte = mem.load8(at + 2u + static_cast<std::uint32_t>(i));
            for (int half = 0; half < 2; ++half) {
                const int nibble = half == 0 ? (byte & 0x0F) : (byte >> 4);
                int sample = static_cast<std::int16_t>(static_cast<std::uint16_t>(nibble << 12)) >> shift;
                sample += (s1_ * c0 + s2_ * c1) >> 6;
                sample = std::clamp(sample, -32768, 32767);
                s2_ = s1_;
                s1_ = sample;
                samples_[static_cast<std::size_t>(i * 2 + half)] = static_cast<std::int16_t>(sample);
            }
        }
        index_ = 0;
        ++block_;
    }

    std::uint32_t data_{}, blocks_{}, block_{}, loop_start_{};
    bool loop_{}, loop_next_{}, end_{true};
    int s1_{}, s2_{};
    std::array<std::int16_t, 28> samples_{};
    int index_{28};
};

// ------------------------------------------------------------------ ADSR envelope

enum class Curve { LinearIncrease, LinearDecrease, LinearBent, ExponentDecrease };

int simple_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    const int rate = ((7 - (n & 3)) << 26) >> (n >> 2);
    return rate == 0 ? 1 : rate;
}

int exponent_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    const int rate = ((7 - (n & 3)) << 24) >> (n >> 2);
    return rate == 0 ? 1 : rate;
}

struct Envelope {
    enum class State { Off, Attack, Decay, Sustain, Release };

    int attack_rate{}, decay_rate{}, sustain_rate{}, release_rate{};
    Curve attack_curve{Curve::LinearIncrease}, sustain_curve{Curve::LinearDecrease},
        release_curve{Curve::LinearDecrease};
    std::int64_t sustain_level{};
    State state{State::Off};
    std::int64_t height{};

    void set_simple(std::uint32_t env1, std::uint32_t env2) {
        attack_rate = simple_rate(static_cast<int>(env1 >> 8));
        attack_curve = (env1 & 0x8000u) != 0 ? Curve::LinearBent : Curve::LinearIncrease;
        const int decay = static_cast<int>((env1 >> 4) & 0xFu);
        decay_rate = decay == 0 ? 0x7FFFFFFF : static_cast<int>(0x80000000u >> decay);
        sustain_level = static_cast<std::int64_t>((env1 & 0xFu) + 1u) << 26;
        switch ((env2 >> 13) & 7u) {
        case 0: sustain_curve = Curve::LinearIncrease; break;
        case 4: sustain_curve = Curve::LinearBent; break;
        case 6: sustain_curve = Curve::ExponentDecrease; break;
        default: sustain_curve = Curve::LinearDecrease; break;
        }
        sustain_rate = sustain_curve == Curve::ExponentDecrease ? exponent_rate(static_cast<int>(env2 >> 6))
                                                                : simple_rate(static_cast<int>(env2 >> 6));
        release_curve = (env2 & 0x20u) != 0 ? Curve::ExponentDecrease : Curve::LinearDecrease;
        const int release = static_cast<int>(env2 & 0x1Fu);
        if (release == 31) {
            release_rate = 0;
        } else if (release_curve == Curve::LinearDecrease) {
            release_rate = release == 30 ? 0x40000000 : release == 29 ? 1 : (0x10000000 >> release);
        } else {
            release_rate = release == 0 ? 0x7FFFFFFF : static_cast<int>(0x80000000u >> release);
        }
    }

    void walk(Curve curve, int rate) {
        switch (curve) {
        case Curve::LinearIncrease: height += rate; break;
        case Curve::LinearDecrease: height -= rate; break;
        case Curve::LinearBent: height += height <= kEnvelopeMax * 3 / 4 ? rate : rate / 4; break;
        case Curve::ExponentDecrease: height -= (height * static_cast<std::int64_t>(rate)) >> 32; break;
        }
    }

    void key_on() {
        state = State::Attack;
        height = 0;
    }
    void key_off() {
        if (state != State::Off) state = State::Release;
    }

    // Advances one sample.
    void step() {
        switch (state) {
        case State::Off: return;
        case State::Attack:
            walk(attack_curve, attack_rate);
            if (height >= kEnvelopeMax) {
                height = kEnvelopeMax;
                state = State::Decay;
            }
            break;
        case State::Decay:
            walk(Curve::ExponentDecrease, decay_rate);
            if (height <= sustain_level) {
                height = sustain_level;
                state = State::Sustain;
            }
            break;
        case State::Sustain:
            walk(sustain_curve, sustain_rate);
            if (height <= 0) {
                height = 0;
                state = State::Release;
            }
            break;
        case State::Release:
            walk(release_curve, release_rate);
            if (height <= 0 || release_rate == 0x40000000) {
                height = 0;
                state = State::Off;
            }
            break;
        }
        height = std::clamp<std::int64_t>(height, 0, kEnvelopeMax);
    }
};

// ------------------------------------------------------------------ voices and the core

struct Voice {
    std::uint32_t vag{}, size{};
    bool loop{};
    bool playing{};
    int pitch{0x1000};
    int left{}, right{}, effect_left{}, effect_right{};
    Envelope envelope;
    VagDecoder decoder;
    // Resampling: the two samples around the playback position and the 12-bit fraction between them.
    int previous{}, current{};
    std::uint32_t fraction{};
};

struct SasCore {
    std::uint32_t grain{256};
    std::uint32_t voices{kMaxVoices};
    std::uint32_t output_mode{};
    std::array<Voice, kMaxVoices> voice{};
    std::vector<std::int32_t> mix;
};

std::unordered_map<std::uint32_t, SasCore> g_cores;

SasCore &core(Context &ctx) { return g_cores[arg(ctx, 0)]; }

Voice *voice(Context &ctx) {
    const int n = sarg(ctx, 1);
    if (n < 0 || n >= kMaxVoices) return nullptr;
    return &core(ctx).voice[static_cast<std::size_t>(n)];
}

// __sceSasInit(core, grainSamples, maxVoices, outputMode, sampleRate)
void SasInit(Runtime &, Context &ctx) {
    SasCore &c = core(ctx);
    c = SasCore{};
    c.grain = std::clamp<std::uint32_t>(arg(ctx, 1), 0x40, 0x800);
    c.voices = std::clamp<std::uint32_t>(arg(ctx, 2), 1, kMaxVoices);
    c.output_mode = arg(ctx, 3);
    ret(ctx, 0);
}

// __sceSasSetVoice(core, voice, vagAddress, size, loop)
void SasSetVoice(Runtime &, Context &ctx) {
    Voice *v = voice(ctx);
    if (v == nullptr) {
        ret(ctx, kErrorInvalidVoice);
        return;
    }
    const std::uint32_t size = arg(ctx, 3);
    if (size == 0 || (size & 0xFu) != 0 || (arg(ctx, 4) & ~1u) != 0) {
        ret(ctx, kErrorInvalidSize);
        return;
    }
    v->vag = arg(ctx, 2);
    v->size = size;
    v->loop = arg(ctx, 4) != 0;
    ret(ctx, 0);
}

void SasSetPitch(Runtime &, Context &ctx) {
    Voice *v = voice(ctx);
    if (v == nullptr) {
        ret(ctx, kErrorInvalidVoice);
        return;
    }
    const int pitch = sarg(ctx, 2);
    if (pitch < 1 || pitch > 0x4000) {
        ret(ctx, kErrorInvalidPitch);
        return;
    }
    v->pitch = pitch;
    ret(ctx, 0);
}

// __sceSasSetVolume(core, voice, left, right, effectLeft, effectRight)
void SasSetVolume(Runtime &, Context &ctx) {
    Voice *v = voice(ctx);
    if (v == nullptr) {
        ret(ctx, kErrorInvalidVoice);
        return;
    }
    for (unsigned i = 2; i <= 5; ++i) {
        const int volume = sarg(ctx, i);
        if (volume < -0x1000 || volume > 0x1000) {
            ret(ctx, kErrorInvalidVolume);
            return;
        }
    }
    v->left = sarg(ctx, 2);
    v->right = sarg(ctx, 3);
    v->effect_left = sarg(ctx, 4);
    v->effect_right = sarg(ctx, 5);
    ret(ctx, 0);
}

void SasSetSimpleADSR(Runtime &, Context &ctx) {
    Voice *v = voice(ctx);
    if (v == nullptr) {
        ret(ctx, kErrorInvalidVoice);
        return;
    }
    v->envelope.set_simple(arg(ctx, 2), arg(ctx, 3));
    ret(ctx, 0);
}

void SasSetKeyOn(Runtime &, Context &ctx) {
    Voice *v = voice(ctx);
    if (v == nullptr) {
        ret(ctx, kErrorInvalidVoice);
        return;
    }
    v->decoder.start(v->vag, v->size, v->loop);
    v->envelope.key_on();
    v->previous = v->current = 0;
    v->fraction = 0;
    v->playing = v->vag != 0;
    ret(ctx, 0);
}

void SasSetKeyOff(Runtime &, Context &ctx) {
    Voice *v = voice(ctx);
    if (v == nullptr) {
        ret(ctx, kErrorInvalidVoice);
        return;
    }
    v->envelope.key_off();
    ret(ctx, 0);
}

// Bit n set: voice n is not playing.
void SasGetEndFlag(Runtime &, Context &ctx) {
    const SasCore &c = core(ctx);
    std::uint32_t flags = 0;
    for (int i = 0; i < kMaxVoices; ++i) {
        if (!c.voice[static_cast<std::size_t>(i)].playing) flags |= 1u << i;
    }
    ret(ctx, flags);
}

void SasGetOutputmode(Runtime &, Context &ctx) { ret(ctx, core(ctx).output_mode); }

// Reverb is not mixed yet: accept its settings.
void SasReverbZero(Runtime &, Context &ctx) { ret(ctx, 0); }

void mix_voice(psprecomp::GuestMemory &mem, Voice &v, std::vector<std::int32_t> &mix, std::uint32_t frames) {
    for (std::uint32_t i = 0; i < frames; ++i) {
        v.fraction += static_cast<std::uint32_t>(v.pitch);
        while (v.fraction >= 0x1000u) {
            v.fraction -= 0x1000u;
            v.previous = v.current;
            v.current = v.decoder.next(mem);
        }
        int sample = v.previous + (((v.current - v.previous) * static_cast<int>(v.fraction)) >> 12);
        sample = static_cast<int>((static_cast<std::int64_t>(sample) * v.envelope.height) >> 30);
        v.envelope.step();
        mix[i * 2u] += (sample * v.left) >> 12;
        mix[i * 2u + 1u] += (sample * v.right) >> 12;
        if (v.envelope.state == Envelope::State::Off || v.decoder.ended()) {
            v.playing = false;
            v.envelope.state = Envelope::State::Off;
            return;
        }
    }
}

// __sceSasCore(core, out): mixes one grain of stereo 16-bit samples.
void SasCoreMix(Runtime &rt, Context &ctx) {
    SasCore &c = core(ctx);
    const std::uint32_t out = arg(ctx, 1);
    c.mix.assign(static_cast<std::size_t>(c.grain) * 2u, 0);
    auto &mem = rt.memory();
    for (std::uint32_t i = 0; i < c.voices; ++i) {
        Voice &v = c.voice[i];
        if (v.playing) mix_voice(mem, v, c.mix, c.grain);
    }
    for (std::uint32_t i = 0; i < c.grain * 2u; ++i) {
        mem.store16(out + i * 2u, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp(c.mix[i], -32768, 32767))));
    }
    ret(ctx, 0);
}

const HleFunction kFunctions[] = {
    {"sceSasCore", 0x42778A9Fu, "__sceSasInit", &SasInit},
    {"sceSasCore", 0xA3589D81u, "__sceSasCore", &SasCoreMix},
    {"sceSasCore", 0x68A46B95u, "__sceSasGetEndFlag", &SasGetEndFlag},
    {"sceSasCore", 0xE175EF66u, "__sceSasGetOutputmode", &SasGetOutputmode},
    {"sceSasCore", 0x99944089u, "__sceSasSetVoice", &SasSetVoice},
    {"sceSasCore", 0xAD84D37Fu, "__sceSasSetPitch", &SasSetPitch},
    {"sceSasCore", 0x440CA7D8u, "__sceSasSetVolume", &SasSetVolume},
    {"sceSasCore", 0xCBCD4F79u, "__sceSasSetSimpleADSR", &SasSetSimpleADSR},
    {"sceSasCore", 0x76F01ACAu, "__sceSasSetKeyOn", &SasSetKeyOn},
    {"sceSasCore", 0xA0CF2FA4u, "__sceSasSetKeyOff", &SasSetKeyOff},
    {"sceSasCore", 0x267A6DD2u, "__sceSasRevParam", &SasReverbZero},
    {"sceSasCore", 0x33D4AB37u, "__sceSasRevType", &SasReverbZero},
    {"sceSasCore", 0xD5A229C9u, "__sceSasRevEVOL", &SasReverbZero},
    {"sceSasCore", 0xF983B186u, "__sceSasRevVON", &SasReverbZero},
};

} // namespace

std::span<const HleFunction> sas_functions() { return kFunctions; }

} // namespace mhf::psp::hle
