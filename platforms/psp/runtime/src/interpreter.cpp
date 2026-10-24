// Fallback interpreter (docs/04-recompiler.md §7): runs guest code the static analysis did not reach, so that a
// missing function is a slowdown and a log line instead of a crash. Calls into recompiled functions go native.
// Coverage: integer, multiply/divide, loads/stores (incl. unaligned), branches and jumps, FPU, and the VFPU
// operations (interpreter_vfpu.cpp, mirroring the emitter); anything else stops with a clear message.

#include "interpreter.hpp"

#include "kernel.hpp"

#include "mhf/psp/abi.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/decoder.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>

namespace mhf::psp {

namespace {

using psprecomp::DecodedInstruction;
using K = psprecomp::OpcodeKind;

struct Interpreter {
    Runtime &rt;
    Context &ctx;
    psprecomp::GuestMemory &mem;

    std::uint32_t r(std::uint32_t i) const { return i == 0 ? 0u : ctx.gpr[i]; }
    void w(std::uint32_t i, std::uint32_t v) {
        if (i != 0) ctx.gpr[i] = v;
    }

    // Executes one non-control-flow instruction.
    void exec(const DecodedInstruction &d, std::uint32_t pc) {
        const auto imm = static_cast<std::int32_t>(d.immediate);
        const auto uimm = static_cast<std::uint32_t>(static_cast<std::uint16_t>(d.immediate));
        const std::uint32_t ea = r(d.rs) + static_cast<std::uint32_t>(imm);
        const std::uint32_t funct = d.word & 0x3Fu;
        switch (d.kind) {
        case K::Nop: case K::Sync: case K::Cache: return;
        case K::Addiu: w(d.rt, r(d.rs) + static_cast<std::uint32_t>(imm)); return;
        case K::Slti: w(d.rt, static_cast<std::int32_t>(r(d.rs)) < imm ? 1u : 0u); return;
        case K::Sltiu: w(d.rt, r(d.rs) < static_cast<std::uint32_t>(imm) ? 1u : 0u); return;
        case K::Andi: w(d.rt, r(d.rs) & uimm); return;
        case K::Ori: w(d.rt, r(d.rs) | uimm); return;
        case K::Xori: w(d.rt, r(d.rs) ^ uimm); return;
        case K::Lui: w(d.rt, uimm << 16u); return;
        case K::Add: case K::Addu: w(d.rd, r(d.rs) + r(d.rt)); return;
        case K::Sub: case K::Subu: w(d.rd, r(d.rs) - r(d.rt)); return;
        case K::And: w(d.rd, r(d.rs) & r(d.rt)); return;
        case K::Or: w(d.rd, r(d.rs) | r(d.rt)); return;
        case K::Xor: w(d.rd, r(d.rs) ^ r(d.rt)); return;
        case K::Nor: w(d.rd, ~(r(d.rs) | r(d.rt))); return;
        case K::Slt: w(d.rd, static_cast<std::int32_t>(r(d.rs)) < static_cast<std::int32_t>(r(d.rt)) ? 1u : 0u); return;
        case K::Sltu: w(d.rd, r(d.rs) < r(d.rt) ? 1u : 0u); return;
        case K::Max: w(d.rd, static_cast<std::int32_t>(r(d.rs)) > static_cast<std::int32_t>(r(d.rt)) ? r(d.rs) : r(d.rt)); return;
        case K::Min: w(d.rd, static_cast<std::int32_t>(r(d.rs)) < static_cast<std::int32_t>(r(d.rt)) ? r(d.rs) : r(d.rt)); return;
        case K::Movz: if (r(d.rt) == 0) w(d.rd, r(d.rs)); return;
        case K::Movn: if (r(d.rt) != 0) w(d.rd, r(d.rs)); return;
        case K::Sll: w(d.rd, r(d.rt) << d.sa); return;
        case K::Srl: w(d.rd, r(d.rt) >> d.sa); return;
        case K::Sra: w(d.rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(r(d.rt)) >> d.sa)); return;
        case K::Rotr: w(d.rd, std::rotr(r(d.rt), static_cast<int>(d.sa))); return;
        case K::Sllv: w(d.rd, r(d.rt) << (r(d.rs) & 31u)); return;
        case K::Srlv: w(d.rd, r(d.rt) >> (r(d.rs) & 31u)); return;
        case K::Srav: w(d.rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(r(d.rt)) >> (r(d.rs) & 31u))); return;
        case K::Rotrv: w(d.rd, std::rotr(r(d.rt), static_cast<int>(r(d.rs) & 31u))); return;
        case K::Clz: w(d.rd, static_cast<std::uint32_t>(std::countl_zero(r(d.rs)))); return;
        case K::Clo: w(d.rd, static_cast<std::uint32_t>(std::countl_one(r(d.rs)))); return;
        case K::Ext: {
            const std::uint32_t size = d.rd + 1u;
            w(d.rt, (r(d.rs) >> d.sa) & (size == 32 ? 0xFFFFFFFFu : (1u << size) - 1u));
            return;
        }
        case K::Ins: {
            const std::uint32_t size = d.rd >= d.sa ? d.rd - d.sa + 1u : 0u;
            const std::uint32_t mask = size == 32 ? 0xFFFFFFFFu : ((1u << size) - 1u);
            w(d.rt, (r(d.rt) & ~(mask << d.sa)) | ((r(d.rs) & mask) << d.sa));
            return;
        }
        case K::Seb: w(d.rd, static_cast<std::uint32_t>(static_cast<std::int8_t>(r(d.rt)))); return;
        case K::Seh: w(d.rd, static_cast<std::uint32_t>(static_cast<std::int16_t>(r(d.rt)))); return;
        case K::Bitrev: {
            std::uint32_t v = r(d.rt), o = 0;
            for (int i = 0; i < 32; ++i) o |= ((v >> i) & 1u) << (31 - i);
            w(d.rd, o);
            return;
        }
        case K::Wsbh: w(d.rd, ((r(d.rt) & 0x00FF00FFu) << 8u) | ((r(d.rt) & 0xFF00FF00u) >> 8u)); return;
        case K::Wsbw: { const std::uint32_t v = r(d.rt); w(d.rd, (v >> 24u) | ((v >> 8u) & 0xFF00u) | ((v << 8u) & 0xFF0000u) | (v << 24u)); return; }
        case K::Lw: w(d.rt, mem.load32(ea)); return;
        case K::Lwl: w(d.rt, mem.load_word_left(ea, r(d.rt))); return;
        case K::Lwr: w(d.rt, mem.load_word_right(ea, r(d.rt))); return;
        case K::Sw: mem.store32(ea, r(d.rt)); return;
        case K::Swl: mem.store_word_left(ea, r(d.rt)); return;
        case K::Swr: mem.store_word_right(ea, r(d.rt)); return;
        case K::Lh: w(d.rt, static_cast<std::uint32_t>(static_cast<std::int16_t>(mem.load16(ea)))); return;
        case K::Lhu: w(d.rt, mem.load16(ea)); return;
        case K::Sh: mem.store16(ea, static_cast<std::uint16_t>(r(d.rt))); return;
        case K::Lb: w(d.rt, static_cast<std::uint32_t>(static_cast<std::int8_t>(mem.load8(ea)))); return;
        case K::Lbu: w(d.rt, mem.load8(ea)); return;
        case K::Sb: mem.store8(ea, static_cast<std::uint8_t>(r(d.rt))); return;
        case K::Lwc1: ctx.set_fpr_bits(d.rt, mem.load32(ea)); return;
        case K::Swc1: mem.store32(ea, ctx.fpr_bits(d.rt)); return;
        case K::Mfhi: w(d.rd, ctx.hi); return;
        case K::Mflo: w(d.rd, ctx.lo); return;
        case K::Mthi: ctx.hi = r(d.rs); return;
        case K::Mtlo: ctx.lo = r(d.rs); return;
        case K::Mult: {
            const auto p = static_cast<std::int64_t>(static_cast<std::int32_t>(r(d.rs))) *
                           static_cast<std::int32_t>(r(d.rt));
            ctx.lo = static_cast<std::uint32_t>(p);
            ctx.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(p) >> 32u);
            return;
        }
        case K::Multu: {
            const std::uint64_t p = static_cast<std::uint64_t>(r(d.rs)) * r(d.rt);
            ctx.lo = static_cast<std::uint32_t>(p);
            ctx.hi = static_cast<std::uint32_t>(p >> 32u);
            return;
        }
        case K::Div: {
            const auto a = static_cast<std::int32_t>(r(d.rs)), b = static_cast<std::int32_t>(r(d.rt));
            if (b == 0) { ctx.lo = a >= 0 ? 0xFFFFFFFFu : 1u; ctx.hi = static_cast<std::uint32_t>(a); }
            else if (a == std::numeric_limits<std::int32_t>::min() && b == -1) { ctx.lo = 0x80000000u; ctx.hi = 0; }
            else { ctx.lo = static_cast<std::uint32_t>(a / b); ctx.hi = static_cast<std::uint32_t>(a % b); }
            return;
        }
        case K::Divu: {
            const std::uint32_t a = r(d.rs), b = r(d.rt);
            if (b == 0) { ctx.lo = 0xFFFFFFFFu; ctx.hi = a; }
            else { ctx.lo = a / b; ctx.hi = a % b; }
            return;
        }
        case K::Mfc1: w(d.rt, ctx.fpr_bits(d.rd)); return;
        case K::Mtc1: ctx.set_fpr_bits(d.rd, r(d.rt)); return;
        case K::Cfc1: w(d.rt, d.rd == 31 ? ctx.fcr31 : 0u); return;
        case K::Ctc1: if (d.rd == 31) ctx.fcr31 = r(d.rt) & 0x0181FFFFu; return;
        case K::AddS: ctx.fpr[d.sa] = ctx.fpr[d.rd] + ctx.fpr[d.rt]; return;
        case K::SubS: ctx.fpr[d.sa] = ctx.fpr[d.rd] - ctx.fpr[d.rt]; return;
        case K::MulS: ctx.fpr[d.sa] = ctx.fpr[d.rd] * ctx.fpr[d.rt]; return;
        case K::DivS: ctx.fpr[d.sa] = ctx.fpr[d.rd] / ctx.fpr[d.rt]; return;
        case K::SqrtS: ctx.fpr[d.sa] = std::sqrt(ctx.fpr[d.rd]); return;
        case K::AbsS: ctx.set_fpr_bits(d.sa, ctx.fpr_bits(d.rd) & 0x7FFFFFFFu); return;
        case K::MovS: ctx.set_fpr_bits(d.sa, ctx.fpr_bits(d.rd)); return;
        case K::NegS: ctx.set_fpr_bits(d.sa, ctx.fpr_bits(d.rd) ^ 0x80000000u); return;
        case K::RoundWS: ctx.set_fpr_bits(d.sa, ctx.fpu_float_to_word(ctx.fpr[d.rd], 0u)); return;
        case K::TruncWS: ctx.set_fpr_bits(d.sa, ctx.fpu_float_to_word(ctx.fpr[d.rd], 1u)); return;
        case K::CeilWS: ctx.set_fpr_bits(d.sa, ctx.fpu_float_to_word(ctx.fpr[d.rd], 2u)); return;
        case K::FloorWS: ctx.set_fpr_bits(d.sa, ctx.fpu_float_to_word(ctx.fpr[d.rd], 3u)); return;
        case K::CvtWS: ctx.set_fpr_bits(d.sa, ctx.fpu_float_to_word(ctx.fpr[d.rd], 4u)); return;
        case K::CvtSW: ctx.fpr[d.sa] = static_cast<float>(static_cast<std::int32_t>(ctx.fpr_bits(d.rd))); return;
        case K::FpuCompare: {
            const float fs = ctx.fpr[d.rd], ft = ctx.fpr[d.rt];
            const bool un = std::isnan(fs) || std::isnan(ft);
            bool c = false;
            switch (d.word & 7u) {
            case 0: c = false; break;
            case 1: c = un; break;
            case 2: c = !un && fs == ft; break;
            case 3: c = un || fs == ft; break;
            case 4: c = fs < ft; break;
            case 5: c = un || fs < ft; break;
            case 6: c = fs <= ft; break;
            default: c = un || fs <= ft; break;
            }
            ctx.set_fpu_condition(c);
            return;
        }
        case K::Mfv: w(d.rt, ctx.vfpu_scalar_bits(d.word & 0xFFu)); return;
        case K::Mtv: ctx.set_vfpu_scalar_bits(d.word & 0xFFu, r(d.rt)); return;
        case K::Lvs: {
            const std::uint32_t reg = ((d.word >> 16u) & 0x1Fu) | ((d.word & 3u) << 5u);
            ctx.set_vfpu_scalar_bits(reg, mem.load32(r(d.rs) + static_cast<std::uint32_t>(
                                                         static_cast<std::int32_t>(static_cast<std::int16_t>(d.word & 0xFFFCu)))));
            return;
        }
        case K::Svs: {
            const std::uint32_t reg = ((d.word >> 16u) & 0x1Fu) | ((d.word & 3u) << 5u);
            mem.store32(r(d.rs) + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(d.word & 0xFFFCu))),
                        ctx.vfpu_scalar_bits(reg));
            return;
        }
        case K::Lvq: case K::Svq: {
            const std::uint32_t reg = ((d.word >> 16u) & 0x1Fu) | ((d.word & 1u) << 5u);
            const std::uint32_t a = r(d.rs) + static_cast<std::uint32_t>(
                                                  static_cast<std::int32_t>(static_cast<std::int16_t>(d.word & 0xFFFCu)));
            float v[4];
            if (d.kind == K::Lvq) {
                for (std::uint32_t i = 0; i < 4; ++i) v[i] = std::bit_cast<float>(mem.load32(a + i * 4u));
                ctx.write_vfpu_vector(v, reg, 4u);
            } else {
                ctx.read_vfpu_vector(v, reg, 4u);
                for (std::uint32_t i = 0; i < 4; ++i) mem.store32(a + i * 4u, std::bit_cast<std::uint32_t>(v[i]));
            }
            return;
        }
        case K::Vpfx: {
            const std::uint32_t control = (d.word >> 24u) & 3u;
            ctx.vfpu_ctrl[control] = d.word & (control == 2 ? 0x00000FFFu : 0x000FFFFFu);
            return;
        }
        case K::Vflush:
            if ((d.word & 0xFFFF0000u) != 0xFFFF0000u) ctx.eat_vfpu_prefixes();
            return;
        default:
            break;
        }
        if (interpret_vfpu(ctx, mem, d, r((d.word >> 21u) & 31u))) return;
        // Allegrex multiply-accumulate (not classified by the vendored decoder).
        if ((d.word >> 26u) == 0u && (funct == 0x1Cu || funct == 0x1Du || funct == 0x2Eu || funct == 0x2Fu)) {
            const bool is_signed = funct == 0x1Cu || funct == 0x2Eu;
            const std::int64_t a = is_signed ? static_cast<std::int32_t>(r(d.rs)) : static_cast<std::int64_t>(r(d.rs));
            const std::int64_t b = is_signed ? static_cast<std::int32_t>(r(d.rt)) : static_cast<std::int64_t>(r(d.rt));
            const std::uint64_t acc = (static_cast<std::uint64_t>(ctx.hi) << 32u) | ctx.lo;
            const auto product = static_cast<std::uint64_t>(a * b);
            const std::uint64_t res = (funct >= 0x2Eu) ? acc - product : acc + product;
            ctx.lo = static_cast<std::uint32_t>(res);
            ctx.hi = static_cast<std::uint32_t>(res >> 32u);
            return;
        }
        rt.fatal("interpreter: instruction " + psprecomp::hex32(d.word) + " (" + d.mnemonic + ") at " +
                 psprecomp::hex32(pc) + " is not interpreted yet");
    }

    [[nodiscard]] bool condition(const DecodedInstruction &d) const {
        switch (d.kind) {
        case K::Beq: case K::Beql: return r(d.rs) == r(d.rt);
        case K::Bne: case K::Bnel: return r(d.rs) != r(d.rt);
        case K::Blez: case K::Blezl: return static_cast<std::int32_t>(r(d.rs)) <= 0;
        case K::Bgtz: case K::Bgtzl: return static_cast<std::int32_t>(r(d.rs)) > 0;
        case K::Bltz: case K::Bltzl: case K::Bltzal: case K::Bltzall: return static_cast<std::int32_t>(r(d.rs)) < 0;
        case K::Bgez: case K::Bgezl: case K::Bgezal: case K::Bgezall: return static_cast<std::int32_t>(r(d.rs)) >= 0;
        case K::Bc1f: case K::Bc1fl: return !ctx.fpu_condition();
        case K::Bc1t: case K::Bc1tl: return ctx.fpu_condition();
        case K::Bvf: case K::Bvfl: return ((ctx.vfpu_ctrl[3] >> ((d.word >> 18u) & 7u)) & 1u) == 0u;
        case K::Bvt: case K::Bvtl: return ((ctx.vfpu_ctrl[3] >> ((d.word >> 18u) & 7u)) & 1u) != 0u;
        default: return false;
        }
    }

    static bool likely(K k) {
        return k == K::Beql || k == K::Bnel || k == K::Blezl || k == K::Bgtzl || k == K::Bltzl || k == K::Bgezl ||
               k == K::Bltzall || k == K::Bgezall || k == K::Bc1fl || k == K::Bc1tl || k == K::Bvfl || k == K::Bvtl;
    }
    static bool link(K k) { return k == K::Bltzal || k == K::Bgezal || k == K::Bltzall || k == K::Bgezall; }

    // Calls `target` the way the guest would: native when recompiled, otherwise interpreted in place.
    // Returns true when the call was made natively (the caller continues after it).
    bool native_call(std::uint32_t target) {
        if (const GuestFunction f = rt.lookup(target & 0x3FFFFFFFu)) {
            f(rt, ctx);
            return true;
        }
        return false;
    }

    void run(std::uint32_t entry) {
        const std::uint32_t return_address = ctx.gpr[31];
        const std::uint32_t entry_sp = ctx.gpr[29];
        std::uint32_t pc = entry;
        for (;;) {
            const auto d = psprecomp::decode_allegrex(mem.load32(pc));
            if (!d.has_delay_slot() && d.kind != K::Jr && d.kind != K::Jalr) {
                exec(d, pc);
                pc += 4u;
                continue;
            }
            const auto slot = psprecomp::decode_allegrex(mem.load32(pc + 4u));
            const std::uint32_t after = pc + 8u;
            if (d.kind == K::J || d.kind == K::Jal) {
                const std::uint32_t target = ((pc + 4u) & 0xF0000000u) | (d.target << 2u);
                if (d.kind == K::Jal) w(31, after);
                exec(slot, pc + 4u);
                if (d.kind == K::Jal) {
                    pc = native_call(target) ? after : target;
                } else if (rt.lookup(target & 0x3FFFFFFFu) != nullptr) {
                    native_call(target); // tail call into recompiled code: it returns to our caller
                    return;
                } else {
                    if (target <= pc) abi::poll_interrupts(rt); // a backward jump may be a busy wait
                    pc = target;
                }
                continue;
            }
            if (d.kind == K::Jr || d.kind == K::Jalr) {
                const std::uint32_t target = r(d.rs);
                if (d.kind == K::Jalr) w(d.rd == 0 ? 31u : d.rd, after);
                exec(slot, pc + 4u);
                if (d.kind == K::Jalr) {
                    pc = native_call(target) ? after : target;
                    continue;
                }
                if (target == return_address && ctx.gpr[29] >= entry_sp) {
                    return; // returned to the caller of the interpreted function
                }
                if (rt.lookup(target & 0x3FFFFFFFu) != nullptr) {
                    native_call(target);
                    return;
                }
                pc = target;
                continue;
            }
            // Conditional branches.
            const bool taken = condition(d);
            const std::uint32_t target = pc + 4u + static_cast<std::uint32_t>(static_cast<std::int32_t>(d.immediate) * 4);
            if (link(d.kind)) w(31, after);
            if (!likely(d.kind) || taken) exec(slot, pc + 4u);
            if (taken && link(d.kind)) {
                pc = native_call(target) ? after : target;
                continue;
            }
            // A loop that waits for something an interrupt or another thread does must still see it happen, as in the recompiled
            // code (which polls on every backward branch).
            if (taken && target <= pc) abi::poll_interrupts(rt);
            pc = taken ? target : after;
        }
    }
};

std::set<std::uint32_t> g_reported;

} // namespace

void interpret_call(Runtime &rt, Context &ctx, std::uint32_t address) {
    if (g_reported.insert(address).second) {
        std::fprintf(stderr, "[interp] interpreting %08X (called from $ra=%08X): not reached by the static analysis\n",
                     address, ctx.gpr[31]);
    }
    Interpreter{rt, ctx, rt.memory()}.run(address & 0x3FFFFFFFu);
}

} // namespace mhf::psp
