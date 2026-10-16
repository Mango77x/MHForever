// VFPU part of the fallback interpreter. Mirrors platforms/psp/recomp/instruction_emitter.cpp case by case, with the
// instruction fields read at run time instead of being baked into generated code. Keep the two in sync.

#include "interpreter.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/decoder.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace mhf::psp {

namespace {

using K = psprecomp::OpcodeKind;
using psprecomp::AllegrexContext;

std::uint32_t size_length(std::uint32_t w) { return (((w >> 7u) & 1u) | (((w >> 15u) & 1u) << 1u)) + 1u; }

float half_to_float(std::uint16_t half) {
    const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16u;
    std::uint32_t exponent = (half >> 10u) & 0x1Fu;
    std::uint32_t mantissa = half & 0x03FFu;
    std::uint32_t bits = 0u;
    if (exponent == 0u) {
        if (mantissa == 0u) {
            bits = sign;
        } else {
            std::uint32_t shift = 0u;
            while ((mantissa & 0x0400u) == 0u) {
                mantissa <<= 1u;
                ++shift;
            }
            mantissa &= 0x03FFu;
            bits = sign | ((113u - shift) << 23u) | (mantissa << 13u);
        }
    } else if (exponent == 31u) {
        bits = sign | 0x7F800000u | (mantissa << 13u);
    } else {
        bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
    }
    return std::bit_cast<float>(bits);
}

float unary(std::uint32_t operation, float x) {
    constexpr float kHalfPi = 1.57079632679489661923f;
    switch (operation) {
    case 0u: return x;
    case 1u: return std::fabs(x);
    case 2u: return -x;
    case 4u: return x <= 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    case 5u: return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
    case 16u: return 1.0f / x;
    case 17u: return 1.0f / std::sqrt(x);
    case 18u: return std::sin(x * kHalfPi);
    case 19u: return std::cos(x * kHalfPi);
    case 20u: return std::exp2(x);
    case 21u: return std::log2(x);
    case 22u: return std::fabs(std::sqrt(x));
    case 23u: return std::asin(x) * 0.63661977236758134308f;
    case 24u: return -1.0f / x;
    case 26u: return -std::sin(x * kHalfPi);
    default: return 1.0f / std::exp2(x);
    }
}

constexpr std::uint32_t kConstantBits[32] = {
    0x00000000u, 0x7F7FFFFFu, 0x3FB504F3u, 0x3F3504F3u, 0x3F906EBAu, 0x3F22F983u, 0x3EA2F983u, 0x3F490FDBu,
    0x3FC90FDBu, 0x40490FDBu, 0x402DF854u, 0x3FB8AA3Bu, 0x3EDE5BD9u, 0x3F317218u, 0x40135D8Eu, 0x40C90FDBu,
    0x3F060A92u, 0x3E9A209Bu, 0x40549A78u, 0x3F5DB3D7u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
};

} // namespace

bool interpret_vfpu(Context &ctx, psprecomp::GuestMemory &mem, const psprecomp::DecodedInstruction &d,
                    std::uint32_t rs_value) {
    const std::uint32_t w = d.word;
    const std::uint32_t n = size_length(w);
    const std::uint32_t vd = w & 0x7Fu;
    const std::uint32_t vs = (w >> 8u) & 0x7Fu;
    const std::uint32_t vt = (w >> 16u) & 0x7Fu;
    switch (d.kind) {
    case K::Viim: {
        const float v[1]{static_cast<float>(static_cast<std::int16_t>(w & 0xFFFFu))};
        ctx.write_vfpu_vector_with_destination_prefix(v, (w >> 16u) & 0x7Fu, 1u);
        return true;
    }
    case K::Vfim: {
        const float v[1]{half_to_float(static_cast<std::uint16_t>(w & 0xFFFFu))};
        ctx.write_vfpu_vector_with_destination_prefix(v, (w >> 16u) & 0x7Fu, 1u);
        return true;
    }
    case K::Vf2h: ctx.execute_vfpu_vf2h(vd, vs, n); return true;
    case K::Vh2f: ctx.execute_vfpu_vh2f(vd, vs, n); return true;
    case K::Vf2i: {
        float s[4]{};
        std::int32_t out[4]{};
        ctx.read_vfpu_vector(s, vs, n);
        ctx.apply_vfpu_source_prefix(s, n, 0u);
        const float scale = std::ldexp(1.0f, static_cast<int>((w >> 16u) & 31u));
        const std::uint32_t mode = (w >> 21u) & 31u;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (std::isnan(s[i])) {
                out[i] = std::numeric_limits<std::int32_t>::max();
                continue;
            }
            const double scaled = static_cast<double>(s[i] * scale);
            if (scaled > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
                out[i] = std::numeric_limits<std::int32_t>::max();
            } else if (scaled <= static_cast<double>(std::numeric_limits<std::int32_t>::min())) {
                out[i] = std::numeric_limits<std::int32_t>::min();
            } else {
                double rounded = 0.0;
                switch (mode) {
                case 16u: rounded = AllegrexContext::round_ties_to_even(scaled); break;
                case 17u: rounded = std::trunc(scaled); break;
                case 18u: rounded = std::ceil(scaled); break;
                default: rounded = std::floor(scaled); break;
                }
                out[i] = static_cast<std::int32_t>(rounded);
            }
        }
        const std::uint32_t dprefix = ctx.vfpu_ctrl[2];
        for (std::uint32_t i = 0; i < n; ++i) {
            if (((dprefix >> (8u + i)) & 1u) == 0u)
                ctx.vfpu[AllegrexContext::vfpu_vector_lane_index(vd, n, i)] =
                    std::bit_cast<float>(static_cast<std::uint32_t>(out[i]));
        }
        ctx.eat_vfpu_prefixes();
        return true;
    }
    case K::Vi2f: {
        float s[4]{}, out[4]{};
        ctx.read_vfpu_vector(s, vs, n);
        ctx.apply_vfpu_source_prefix(s, n, 0u);
        const float scale = std::ldexp(1.0f, -static_cast<int>((w >> 16u) & 31u));
        for (std::uint32_t i = 0; i < n; ++i)
            out[i] = static_cast<float>(static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(s[i]))) * scale;
        ctx.write_vfpu_vector_with_destination_prefix(out, vd, n);
        return true;
    }
    case K::Vx2i: ctx.execute_vfpu_vx2i(vd, vs, n, (w >> 16u) & 3u); return true;
    case K::VmidT: ctx.execute_vfpu_matrix_init(vd, n, 3u); return true;
    case K::Vmmov: ctx.execute_vfpu_vmmov(vd, vs, n); return true;
    case K::VfpuMatrixInit: ctx.execute_vfpu_matrix_init(vd, n, (w >> 16u) & 15u); return true;
    case K::Vidt: {
        float v[4]{};
        v[vd & (n >= 3u ? 3u : 1u)] = 1.0f;
        ctx.write_vfpu_vector_with_destination_prefix(v, vd, n);
        return true;
    }
    case K::Vtfm: {
        const std::uint32_t side = ((w >> 23u) & 3u) + 1u;
        float m[16]{}, raw[4]{}, t[4]{}, res[4]{};
        ctx.read_vfpu_matrix(m, vs, side);
        ctx.read_vfpu_vector(raw, vt, side);
        for (std::uint32_t i = 0; i < 4u; ++i) t[i] = i < n ? raw[i] : 0.0f;
        if (side - 1u >= n) t[side - 1u] = 1.0f;
        for (std::uint32_t row = 0; row + 1u < side; ++row) {
            float sum = 0.0f;
            for (std::uint32_t c = 0; c < side; ++c) sum += m[row * 4u + c] * t[c];
            res[row] = sum;
        }
        float last[4]{m[(side - 1u) * 4u + 0u], m[(side - 1u) * 4u + 1u], m[(side - 1u) * 4u + 2u],
                      m[(side - 1u) * 4u + 3u]};
        ctx.apply_vfpu_source_prefix(last, 4u, 0u);
        ctx.apply_vfpu_source_prefix(t, 4u, 1u);
        for (std::uint32_t c = 0; c < 4u; ++c) res[side - 1u] += last[c] * t[c];
        const std::uint32_t dprefix = ctx.vfpu_ctrl[2];
        const std::uint32_t lane = side - 1u;
        ctx.vfpu_ctrl[2] = ((dprefix & (1u << 8u)) << lane) | ((dprefix & 3u) << (lane * 2u));
        ctx.write_vfpu_vector_with_destination_prefix(res, vd, side);
        return true;
    }
    case K::VfpuVectorInit: {
        const bool one = ((w >> 16u) & 31u) == 7u;
        const float x = one ? 1.0f : 0.0f;
        const float v[4]{x, x, x, x};
        ctx.write_vfpu_vector_with_destination_prefix(v, vd, n);
        return true;
    }
    case K::Vmmul: {
        float s[16]{}, t[16]{}, out[16]{};
        ctx.read_vfpu_matrix(s, vs, n);
        ctx.read_vfpu_matrix(t, vt, n);
        for (std::uint32_t a = 0; a < n; ++a)
            for (std::uint32_t b = 0; b < n; ++b) {
                float sum = 0.0f;
                for (std::uint32_t c = 0; c < n; ++c) sum += s[b * 4u + c] * t[a * 4u + c];
                out[a * 4u + b] = sum;
            }
        ctx.write_vfpu_matrix(out, vd, n);
        ctx.eat_vfpu_prefixes();
        return true;
    }
    case K::Vmscl: ctx.execute_vfpu_vmscl(vd, vs, vt, n); return true;
    case K::Vrot: ctx.execute_vfpu_vrot(vd, vs, n, (w >> 16u) & 31u); return true;
    case K::Vocp: ctx.execute_vfpu_vocp(vd, vs, n); return true;
    case K::VfpuHorizontal: ctx.execute_vfpu_horizontal(vd, vs, n, ((w >> 16u) & 31u) == 7u); return true;
    case K::VfpuVec3: {
        const std::uint32_t op = (w >> 26u) == 0x19u ? 2u : ((w >> 23u) & 7u);
        float s[4]{}, t[4]{}, out[4]{};
        ctx.read_vfpu_vector_with_source_prefix(s, vs, n, 0u);
        ctx.read_vfpu_vector_with_source_prefix(t, vt, n, 1u);
        for (std::uint32_t i = 0; i < n; ++i)
            out[i] = op == 0u ? s[i] + t[i] : op == 1u ? s[i] - t[i] : op == 2u ? s[i] * t[i] : s[i] / t[i];
        ctx.write_vfpu_vector_with_destination_prefix(out, vd, n);
        return true;
    }
    case K::Vdot: ctx.execute_vfpu_vdot(vd, vs, vt, n); return true;
    case K::Vhdp: ctx.execute_vfpu_vhdp(vd, vs, vt, n); return true;
    case K::VcrossQuat: ctx.execute_vfpu_cross_quat(vd, vs, vt, n); return true;
    case K::Vminmax: ctx.execute_vfpu_vminmax(vd, vs, vt, n, ((w >> 23u) & 7u) == 3u); return true;
    case K::VfpuCompare3: ctx.execute_vfpu_compare3(vd, vs, vt, n, (w >> 23u) & 7u); return true;
    case K::Vcmp: ctx.execute_vfpu_vcmp(vs, vt, n, w & 15u); return true;
    case K::Vcmov: ctx.execute_vfpu_vcmov(vd, vs, n, (w >> 16u) & 7u, ((w >> 19u) & 1u) != 0u); return true;
    case K::Vscl: ctx.execute_vfpu_vscl(vd, vs, vt, n); return true;
    case K::VfpuUnary: {
        const std::uint32_t op = (w >> 16u) & 31u;
        float s[4]{}, out[4]{};
        ctx.read_vfpu_vector_with_source_prefix(s, vs, n, 0u);
        for (std::uint32_t i = 0; i < n; ++i) out[i] = unary(op, s[i]);
        ctx.write_vfpu_vector_with_destination_prefix(out, vd, n);
        return true;
    }
    case K::Vcst: {
        const float c = std::bit_cast<float>(kConstantBits[(w >> 16u) & 31u]);
        const float v[4]{c, c, c, c};
        ctx.write_vfpu_vector_with_destination_prefix(v, vd, n);
        return true;
    }
    default:
        break;
    }

    // Instructions the vendored decoder does not classify (see emit_extension in the emitter).
    const std::uint32_t op = w >> 26u;
    if (op == 0x35u || op == 0x3Du) {
        const std::uint32_t reg = ((w >> 16u) & 0x1Fu) | ((w & 1u) << 5u);
        const std::uint32_t address =
            rs_value + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(w & 0xFFFCu)));
        const std::uint32_t k = (address >> 2u) & 3u;
        const bool right = (w & 2u) != 0;
        float v[4]{};
        ctx.read_vfpu_vector(v, reg, 4u);
        if (op == 0x35u) {
            if (!right) {
                for (std::uint32_t i = 0; i <= k; ++i) v[3 - i] = std::bit_cast<float>(mem.load32(address - 4u * i));
            } else {
                for (std::uint32_t i = 0; i <= 3u - k; ++i) v[i] = std::bit_cast<float>(mem.load32(address + 4u * i));
            }
            ctx.write_vfpu_vector(v, reg, 4u);
        } else if (!right) {
            for (std::uint32_t i = 0; i <= k; ++i) mem.store32(address - 4u * i, std::bit_cast<std::uint32_t>(v[3 - i]));
        } else {
            for (std::uint32_t i = 0; i <= 3u - k; ++i) mem.store32(address + 4u * i, std::bit_cast<std::uint32_t>(v[i]));
        }
        return true;
    }
    if (op == 0x34u && ((w >> 21u) & 31u) == 2u) {
        const std::uint32_t operation = (w >> 16u) & 31u;
        float s[4]{}, out[4]{};
        if (operation == 10u) {
            ctx.read_vfpu_vector_with_source_prefix(s, vs, n, 0u);
            for (std::uint32_t i = 0; i < n; ++i) out[i] = s[i] > 0.0f ? 1.0f : (s[i] < 0.0f ? -1.0f : 0.0f);
            ctx.write_vfpu_vector_with_destination_prefix(out, vd, n);
            return true;
        }
        if (operation == 5u && n <= 2u) {
            ctx.read_vfpu_vector_with_source_prefix(s, vs, n, 0u);
            for (std::uint32_t i = 0; i < n; ++i) {
                out[2 * i] = std::fmin(1.0f, std::fmax(0.0f, 1.0f - s[i]));
                out[2 * i + 1] = std::fmin(1.0f, std::fmax(0.0f, s[i]));
            }
            ctx.write_vfpu_vector_with_destination_prefix(out, vd, n * 2u);
            return true;
        }
    }
    return false;
}

} // namespace mhf::psp
