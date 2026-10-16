// Allegrex instruction semantics → C++ statements.
//
// Derived from PSPRecomp's tools/codegen_main.cpp (emit_regular and helpers), commit f6e7d41,
// Copyright (c) PSPRecomp contributors, MIT License (see third_party/psprecomp/LICENSE).
// Local changes: moved into a library, namespaced, memory accesses go through a local `mem` reference.

#include "instruction_emitter.hpp"

#include "psprecomp/codegen_policy.hpp"
#include "psprecomp/common.hpp"

#include <cstdint>
#include <sstream>
#include <string>

namespace mhf::psp::recomp {

std::string cpp_escape(const std::string &text) {
    std::string out;
    for (const char c : text) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string reg(std::uint32_t index) {
    // Allegrex/MIPS $zero is immutable. Emitting a literal lets the host
    // compiler fold branches/arithmetic instead of materializing a load from
    // AllegrexContext for every source-register use of r0.
    if (index == 0u) return "0u";
    return "ctx.gpr[" + std::to_string(index) + "]";
}

bool is_multiply_accumulate(std::uint32_t word) {
    const std::uint32_t funct = word & 0x3Fu;
    return (word >> 26u) == 0u && (funct == 0x1Cu || funct == 0x1Du || funct == 0x2Eu || funct == 0x2Fu);
}

namespace {

// Allegrex instructions that the vendored decoder does not classify. Returns an empty string when `d` is not one.
std::string emit_extension(const psprecomp::DecodedInstruction &d, std::uint32_t pc) {
    std::ostringstream out;
    const std::uint32_t rs = (d.word >> 21u) & 31u;
    const std::uint32_t rt = (d.word >> 16u) & 31u;
    if (is_multiply_accumulate(d.word)) {
        const std::uint32_t funct = d.word & 0x3Fu;
        const bool is_signed = funct == 0x1Cu || funct == 0x2Eu;
        const bool subtract = funct == 0x2Eu || funct == 0x2Fu;
        const std::string a = is_signed ? "static_cast<std::int64_t>(static_cast<std::int32_t>(" + reg(rs) + "))"
                                        : "static_cast<std::int64_t>(static_cast<std::uint64_t>(" + reg(rs) + "))";
        const std::string b = is_signed ? "static_cast<std::int64_t>(static_cast<std::int32_t>(" + reg(rt) + "))"
                                        : "static_cast<std::int64_t>(static_cast<std::uint64_t>(" + reg(rt) + "))";
        out << "    { const std::uint64_t acc = (static_cast<std::uint64_t>(ctx.hi) << 32u) | ctx.lo; "
            << "const std::uint64_t product = static_cast<std::uint64_t>(" << a << " * " << b << "); "
            << "const std::uint64_t r = acc " << (subtract ? "-" : "+") << " product; "
            << "ctx.lo = static_cast<std::uint32_t>(r); ctx.hi = static_cast<std::uint32_t>(r >> 32u); }\n";
        return out.str();
    }
    if ((d.word >> 26u) == 0u && (d.word & 0x3Fu) == 0x0Du) {
        out << "    rt.unsupported(" << psprecomp::hex32(pc) << "u, " << psprecomp::hex32(d.word)
            << "u, \"break (guest trap)\"); return;\n";
        return out.str();
    }
    // VFPU4 group 2: vsocp (operation 5) and vsgn (operation 10).
    if ((d.word >> 26u) == 0x34u && ((d.word >> 21u) & 31u) == 2u) {
        const std::uint32_t operation = (d.word >> 16u) & 31u;
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        if (operation == 10u) {
            out << "    { float vfpu_s[4]{}, vfpu_d[4]{};\n"
                << "      ctx.read_vfpu_vector_with_source_prefix(vfpu_s, " << source << "u, " << length << "u, 0u);\n"
                << "      for (std::uint32_t i = 0; i < " << length << "u; ++i) vfpu_d[i] = vfpu_s[i] > 0.0f ? 1.0f : "
                << "(vfpu_s[i] < 0.0f ? -1.0f : 0.0f);\n"
                << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_d, " << destination << "u, " << length
                << "u); }\n";
            return out.str();
        }
        if (operation == 5u && length <= 2u) {
            const std::uint32_t out_length = length * 2u;
            out << "    { float vfpu_s[4]{}, vfpu_d[4]{};\n"
                << "      ctx.read_vfpu_vector_with_source_prefix(vfpu_s, " << source << "u, " << length << "u, 0u);\n"
                << "      for (std::uint32_t i = 0; i < " << length << "u; ++i) {\n"
                << "          const float x = vfpu_s[i];\n"
                << "          vfpu_d[2 * i] = std::fmin(1.0f, std::fmax(0.0f, 1.0f - x));\n"
                << "          vfpu_d[2 * i + 1] = std::fmin(1.0f, std::fmax(0.0f, x)); }\n"
                << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_d, " << destination << "u, " << out_length
                << "u); }\n";
            return out.str();
        }
    }
    return {};
}

} // namespace

bool ends_block(const psprecomp::DecodedInstruction &d) {
    return d.kind == psprecomp::OpcodeKind::Unsupported && !is_multiply_accumulate(d.word);
}

std::string emit_instruction(const psprecomp::DecodedInstruction &d, std::uint32_t pc) {
    if (d.kind == psprecomp::OpcodeKind::Unsupported || d.kind == psprecomp::OpcodeKind::Vfpu) {
        if (auto text = emit_extension(d, pc); !text.empty()) {
            return text;
        }
    }
    const auto imm = static_cast<std::int32_t>(d.immediate);
    const auto uimm = static_cast<std::uint16_t>(d.immediate);
    std::ostringstream out;
    switch (d.kind) {
    case psprecomp::OpcodeKind::Nop: out << "    // nop\n"; break;
    case psprecomp::OpcodeKind::Sync:
    case psprecomp::OpcodeKind::Cache:
        out << psprecomp::codegen::memory_ordering_statement(d.kind);
        break;
    case psprecomp::OpcodeKind::Addiu: out << "    ctx.set_gpr(" << d.rt << ", " << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "));\n"; break;
    case psprecomp::OpcodeKind::Slti: out << "    ctx.set_gpr(" << d.rt << ", static_cast<std::int32_t>(" << reg(d.rs) << ") < " << imm << " ? 1u : 0u);\n"; break;
    case psprecomp::OpcodeKind::Sltiu: out << "    ctx.set_gpr(" << d.rt << ", " << reg(d.rs) << " < static_cast<std::uint32_t>(" << imm << ") ? 1u : 0u);\n"; break;
    case psprecomp::OpcodeKind::Andi: out << "    ctx.set_gpr(" << d.rt << ", " << reg(d.rs) << " & " << uimm << "u);\n"; break;
    case psprecomp::OpcodeKind::Ori: out << "    ctx.set_gpr(" << d.rt << ", " << reg(d.rs) << " | " << uimm << "u);\n"; break;
    case psprecomp::OpcodeKind::Xori: out << "    ctx.set_gpr(" << d.rt << ", " << reg(d.rs) << " ^ " << uimm << "u);\n"; break;
    case psprecomp::OpcodeKind::Lui: out << "    ctx.set_gpr(" << d.rt << ", " << uimm << "u << 16u);\n"; break;
    case psprecomp::OpcodeKind::Add:
        out << "    { const bool signed_ok = ctx.execute_signed_add(" << d.rd << "u, " << d.rs << "u, " << d.rt
            << "u);\n"
            << "      if (!signed_ok) { rt.arithmetic_overflow(" << psprecomp::hex32(pc) << "u, "
            << psprecomp::hex32(d.word) << "u); return; } }\n";
        break;
    case psprecomp::OpcodeKind::Addu: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << " + " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Sub:
        out << "    { const bool signed_ok = ctx.execute_signed_sub(" << d.rd << "u, " << d.rs << "u, " << d.rt
            << "u);\n"
            << "      if (!signed_ok) { rt.arithmetic_overflow(" << psprecomp::hex32(pc) << "u, "
            << psprecomp::hex32(d.word) << "u); return; } }\n";
        break;
    case psprecomp::OpcodeKind::Subu: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << " - " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::And: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << " & " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Or: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << " | " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Xor: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << " ^ " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Nor: out << "    ctx.set_gpr(" << d.rd << ", ~(" << reg(d.rs) << " | " << reg(d.rt) << "));\n"; break;
    case psprecomp::OpcodeKind::Slt: out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::int32_t>(" << reg(d.rs) << ") < static_cast<std::int32_t>(" << reg(d.rt) << ") ? 1u : 0u);\n"; break;
    case psprecomp::OpcodeKind::Sltu: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << " < " << reg(d.rt) << " ? 1u : 0u);\n"; break;
    case psprecomp::OpcodeKind::Max: out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::int32_t>(" << reg(d.rs) << ") > static_cast<std::int32_t>(" << reg(d.rt) << ") ? " << reg(d.rs) << " : " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Min: out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::int32_t>(" << reg(d.rs) << ") < static_cast<std::int32_t>(" << reg(d.rt) << ") ? " << reg(d.rs) << " : " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Movz:
        out << "    if (" << reg(d.rt) << " == 0u) ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << ");\n";
        break;
    case psprecomp::OpcodeKind::Movn:
        out << "    if (" << reg(d.rt) << " != 0u) ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << ");\n";
        break;
    case psprecomp::OpcodeKind::Sll: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rt) << " << " << d.sa << "u);\n"; break;
    case psprecomp::OpcodeKind::Srl: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rt) << " >> " << d.sa << "u);\n"; break;
    case psprecomp::OpcodeKind::Sra: out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::uint32_t>(static_cast<std::int32_t>(" << reg(d.rt) << ") >> " << d.sa << "u));\n"; break;
    case psprecomp::OpcodeKind::Rotr: out << "    ctx.set_gpr(" << d.rd << ", std::rotr(" << reg(d.rt) << ", " << d.sa << "));\n"; break;
    case psprecomp::OpcodeKind::Sllv: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rt) << " << (" << reg(d.rs) << " & 31u));\n"; break;
    case psprecomp::OpcodeKind::Srlv: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rt) << " >> (" << reg(d.rs) << " & 31u));\n"; break;
    case psprecomp::OpcodeKind::Srav: out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::uint32_t>(static_cast<std::int32_t>(" << reg(d.rt) << ") >> (" << reg(d.rs) << " & 31u)));\n"; break;
    case psprecomp::OpcodeKind::Rotrv: out << "    ctx.set_gpr(" << d.rd << ", std::rotr(" << reg(d.rt) << ", static_cast<int>(" << reg(d.rs) << " & 31u)));\n"; break;
    case psprecomp::OpcodeKind::Clz:
        out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::uint32_t>(std::countl_zero(" << reg(d.rs) << ")));\n";
        break;
    case psprecomp::OpcodeKind::Clo:
        out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::uint32_t>(std::countl_one(" << reg(d.rs) << ")));\n";
        break;
    case psprecomp::OpcodeKind::Ext: {
        const std::uint32_t size = d.rd + 1u;
        const std::uint32_t mask = size == 32u ? 0xFFFFFFFFu : ((1u << size) - 1u);
        out << "    ctx.set_gpr(" << d.rt << ", (" << reg(d.rs) << " >> " << d.sa << "u) & " << psprecomp::hex32(mask) << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Ins: {
        const std::uint32_t size = d.rd >= d.sa ? d.rd - d.sa + 1u : 0u;
        const std::uint32_t source_mask = size == 32u ? 0xFFFFFFFFu : (size == 0u ? 0u : ((1u << size) - 1u));
        const std::uint32_t destination_mask = source_mask << d.sa;
        out << "    ctx.set_gpr(" << d.rt << ", (" << reg(d.rt) << " & ~" << psprecomp::hex32(destination_mask)
            << "u) | ((" << reg(d.rs) << " & " << psprecomp::hex32(source_mask) << "u) << " << d.sa << "u));\n";
        break;
    }
    case psprecomp::OpcodeKind::Seb:
        out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(" << reg(d.rt) << "))));\n";
        break;
    case psprecomp::OpcodeKind::Seh:
        out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(" << reg(d.rt) << "))));\n";
        break;
    case psprecomp::OpcodeKind::Bitrev:
        out << "    ctx.set_gpr(" << d.rd << ", [](std::uint32_t value) { "
               "value = ((value >> 1u) & 0x55555555u) | ((value & 0x55555555u) << 1u); "
               "value = ((value >> 2u) & 0x33333333u) | ((value & 0x33333333u) << 2u); "
               "value = ((value >> 4u) & 0x0F0F0F0Fu) | ((value & 0x0F0F0F0Fu) << 4u); "
               "value = ((value >> 8u) & 0x00FF00FFu) | ((value & 0x00FF00FFu) << 8u); "
               "return (value >> 16u) | (value << 16u); }(" << reg(d.rt) << "));\n";
        break;
    case psprecomp::OpcodeKind::Wsbh:
        out << "    ctx.set_gpr(" << d.rd << ", ((" << reg(d.rt) << " & 0x00FF00FFu) << 8u) | ((" << reg(d.rt) << " & 0xFF00FF00u) >> 8u));\n";
        break;
    case psprecomp::OpcodeKind::Wsbw:
        out << "    ctx.set_gpr(" << d.rd << ", ((" << reg(d.rt) << " & 0x000000FFu) << 24u) | ((" << reg(d.rt) << " & 0x0000FF00u) << 8u) | ((" << reg(d.rt) << " & 0x00FF0000u) >> 8u) | ((" << reg(d.rt) << " & 0xFF000000u) >> 24u));\n";
        break;
    case psprecomp::OpcodeKind::Lw: out << "    ctx.set_gpr(" << d.rt << ", mem.aot_load32(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << ")));\n"; break;
    case psprecomp::OpcodeKind::Lwl: out << "    ctx.set_gpr(" << d.rt << ", mem.aot_load_word_left(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), " << reg(d.rt) << "));\n"; break;
    case psprecomp::OpcodeKind::Lwr: out << "    ctx.set_gpr(" << d.rt << ", mem.aot_load_word_right(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), " << reg(d.rt) << "));\n"; break;
    case psprecomp::OpcodeKind::Sw: out << "    mem.aot_store32(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Swl: out << "    mem.aot_store_word_left(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Swr: out << "    mem.aot_store_word_right(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Lh: out << "    ctx.set_gpr(" << d.rt << ", static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(mem.aot_load16(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "))))));\n"; break;
    case psprecomp::OpcodeKind::Lhu: out << "    ctx.set_gpr(" << d.rt << ", mem.aot_load16(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << ")));\n"; break;
    case psprecomp::OpcodeKind::Sh: out << "    mem.aot_store16(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), static_cast<std::uint16_t>(" << reg(d.rt) << "));\n"; break;
    case psprecomp::OpcodeKind::Lb: out << "    ctx.set_gpr(" << d.rt << ", static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(mem.aot_load8(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "))))));\n"; break;
    case psprecomp::OpcodeKind::Lbu: out << "    ctx.set_gpr(" << d.rt << ", mem.aot_load8(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << ")));\n"; break;
    case psprecomp::OpcodeKind::Sb: out << "    mem.aot_store8(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), static_cast<std::uint8_t>(" << reg(d.rt) << "));\n"; break;
    case psprecomp::OpcodeKind::Lwc1: out << "    ctx.fpr[" << d.rt << "] = std::bit_cast<float>(mem.aot_load32(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << ")));\n"; break;
    case psprecomp::OpcodeKind::Swc1: out << "    mem.aot_store32(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), std::bit_cast<std::uint32_t>(ctx.fpr[" << d.rt << "]));\n"; break;
    case psprecomp::OpcodeKind::Mfhi: out << "    ctx.set_gpr(" << d.rd << ", ctx.hi);\n"; break;
    case psprecomp::OpcodeKind::Mflo: out << "    ctx.set_gpr(" << d.rd << ", ctx.lo);\n"; break;
    case psprecomp::OpcodeKind::Mthi: out << "    ctx.hi = " << reg(d.rs) << ";\n"; break;
    case psprecomp::OpcodeKind::Mtlo: out << "    ctx.lo = " << reg(d.rs) << ";\n"; break;
    case psprecomp::OpcodeKind::Mult:
        out << "    { const std::int64_t product = static_cast<std::int64_t>(static_cast<std::int32_t>(" << reg(d.rs) << ")) * "
            << "static_cast<std::int64_t>(static_cast<std::int32_t>(" << reg(d.rt) << ")); "
            << "ctx.lo = static_cast<std::uint32_t>(product); ctx.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(product) >> 32u); }\n";
        break;
    case psprecomp::OpcodeKind::Multu:
        out << "    { const std::uint64_t product = static_cast<std::uint64_t>(" << reg(d.rs) << ") * static_cast<std::uint64_t>(" << reg(d.rt) << "); "
            << "ctx.lo = static_cast<std::uint32_t>(product); ctx.hi = static_cast<std::uint32_t>(product >> 32u); }\n";
        break;
    case psprecomp::OpcodeKind::Div:
        // $zero is emitted as a literal 0u.  If it is also the divisor, do not
        // emit a syntactically present / or % expression at all: MSVC diagnoses
        // division by a compile-time zero even when it sits behind an ordinary
        // `if (divisor == 0)` guard.  This is also cheaper and exactly matches
        // Allegrex/MIPS divide-by-zero HI/LO semantics.
        if (d.rt == 0u) {
            out << "    { const std::int32_t dividend = static_cast<std::int32_t>(" << reg(d.rs) << "); "
                << "ctx.lo = dividend >= 0 ? 0xFFFFFFFFu : 1u; "
                << "ctx.hi = static_cast<std::uint32_t>(dividend); }\n";
        } else {
            out << "    { const std::int32_t dividend = static_cast<std::int32_t>(" << reg(d.rs) << "); "
                << "const std::int32_t divisor = static_cast<std::int32_t>(" << reg(d.rt) << "); "
                << "if (divisor == 0) { ctx.lo = dividend >= 0 ? 0xFFFFFFFFu : 1u; ctx.hi = static_cast<std::uint32_t>(dividend); } "
                << "else if (dividend == static_cast<std::int32_t>(0x80000000u) && divisor == -1) { ctx.lo = 0x80000000u; ctx.hi = 0u; } "
                << "else { ctx.lo = static_cast<std::uint32_t>(dividend / divisor); ctx.hi = static_cast<std::uint32_t>(dividend % divisor); } }\n";
        }
        break;
    case psprecomp::OpcodeKind::Divu:
        if (d.rt == 0u) {
            out << "    { const std::uint32_t dividend = " << reg(d.rs) << "; "
                << "ctx.lo = 0xFFFFFFFFu; ctx.hi = dividend; }\n";
        } else {
            out << "    { const std::uint32_t dividend = " << reg(d.rs) << "; const std::uint32_t divisor = " << reg(d.rt) << "; "
                << "if (divisor == 0u) { ctx.lo = 0xFFFFFFFFu; ctx.hi = dividend; } "
                << "else { ctx.lo = dividend / divisor; ctx.hi = dividend % divisor; } }\n";
        }
        break;
    case psprecomp::OpcodeKind::Mfc1:
        out << "    ctx.set_gpr(" << d.rt << ", std::bit_cast<std::uint32_t>(ctx.fpr[" << d.rd << "]));\n";
        break;
    case psprecomp::OpcodeKind::Mtc1:
        out << "    ctx.fpr[" << d.rd << "] = std::bit_cast<float>(" << reg(d.rt) << ");\n";
        break;
    case psprecomp::OpcodeKind::Cfc1:
        if (d.rd == 31u) out << "    ctx.set_gpr(" << d.rt << ", ctx.fcr31);\n";
        else out << "    rt.unsupported(" << psprecomp::hex32(pc) << "u, " << psprecomp::hex32(d.word) << "u, \"unsupported CFC1 control register\"); return;\n";
        break;
    case psprecomp::OpcodeKind::Ctc1:
        if (d.rd == 31u) out << "    ctx.fcr31 = " << reg(d.rt) << " & 0x0181FFFFu;\n";
        else out << "    rt.unsupported(" << psprecomp::hex32(pc) << "u, " << psprecomp::hex32(d.word) << "u, \"unsupported CTC1 control register\"); return;\n";
        break;
    case psprecomp::OpcodeKind::AddS:
        out << "    ctx.fpr[" << d.sa << "] = ctx.fpr[" << d.rd << "] + ctx.fpr[" << d.rt << "];\n";
        break;
    case psprecomp::OpcodeKind::SubS:
        out << "    ctx.fpr[" << d.sa << "] = ctx.fpr[" << d.rd << "] - ctx.fpr[" << d.rt << "];\n";
        break;
    case psprecomp::OpcodeKind::MulS:
        out << "    { const float fs = ctx.fpr[" << d.rd << "]; const float ft = ctx.fpr[" << d.rt << "]; "
            << "if ((std::isinf(fs) && ft == 0.0f) || (std::isinf(ft) && fs == 0.0f)) ctx.set_fpr_bits(" << d.sa << ", 0x7FC00000u); "
            << "else ctx.fpr[" << d.sa << "] = fs * ft; }\n";
        break;
    case psprecomp::OpcodeKind::DivS:
        out << "    ctx.fpr[" << d.sa << "] = ctx.fpr[" << d.rd << "] / ctx.fpr[" << d.rt << "];\n";
        break;
    case psprecomp::OpcodeKind::SqrtS:
        out << "    ctx.fpr[" << d.sa << "] = std::sqrt(ctx.fpr[" << d.rd << "]);\n";
        break;
    case psprecomp::OpcodeKind::AbsS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpr_bits(" << d.rd << ") & 0x7FFFFFFFu);\n";
        break;
    case psprecomp::OpcodeKind::MovS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpr_bits(" << d.rd << "));\n";
        break;
    case psprecomp::OpcodeKind::NegS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpr_bits(" << d.rd << ") ^ 0x80000000u);\n";
        break;
    case psprecomp::OpcodeKind::RoundWS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpu_float_to_word(ctx.fpr[" << d.rd << "], 0u));\n";
        break;
    case psprecomp::OpcodeKind::TruncWS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpu_float_to_word(ctx.fpr[" << d.rd << "], 1u));\n";
        break;
    case psprecomp::OpcodeKind::CeilWS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpu_float_to_word(ctx.fpr[" << d.rd << "], 2u));\n";
        break;
    case psprecomp::OpcodeKind::FloorWS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpu_float_to_word(ctx.fpr[" << d.rd << "], 3u));\n";
        break;
    case psprecomp::OpcodeKind::CvtWS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpu_float_to_word(ctx.fpr[" << d.rd << "], 4u));\n";
        break;
    case psprecomp::OpcodeKind::CvtSW:
        out << "    ctx.fpr[" << d.sa << "] = static_cast<float>(static_cast<std::int32_t>(ctx.fpr_bits(" << d.rd << ")));\n";
        break;
    case psprecomp::OpcodeKind::FpuCompare: {
        const std::uint32_t condition = d.word & 0xFu;
        const std::string fs = "ctx.fpr[" + std::to_string(d.rd) + "]";
        const std::string ft = "ctx.fpr[" + std::to_string(d.rt) + "]";
        const std::string unordered = "(std::isnan(" + fs + ") || std::isnan(" + ft + "))";
        std::string expression;
        switch (condition) {
        case 0u: case 8u: expression = "false"; break;
        case 1u: case 9u: expression = unordered; break;
        case 2u: case 10u: expression = "(!" + unordered + " && " + fs + " == " + ft + ")"; break;
        case 3u: case 11u: expression = "(" + unordered + " || " + fs + " == " + ft + ")"; break;
        case 4u: case 12u: expression = "(" + fs + " < " + ft + ")"; break;
        case 5u: case 13u: expression = "(" + unordered + " || " + fs + " < " + ft + ")"; break;
        case 6u: case 14u: expression = "(" + fs + " <= " + ft + ")"; break;
        default: expression = "(" + unordered + " || " + fs + " <= " + ft + ")"; break;
        }
        out << "    ctx.set_fpu_condition(" << expression << ");\n";
        break;
    }
    case psprecomp::OpcodeKind::Vflush:
        if ((d.word & 0xFFFF0000u) == 0xFFFF0000u) {
            out << "    // vflush: architectural no-op that retains VFPU prefixes\n";
        } else {
            out << "    ctx.eat_vfpu_prefixes(); // VFPU sync/no-op consumes prefixes\n";
        }
        break;
    case psprecomp::OpcodeKind::Vpfx: {
        const std::uint32_t control = (d.word >> 24u) & 3u;
        std::uint32_t data = d.word & 0x000FFFFFu;
        if (control == 2u) data &= 0x00000FFFu;
        out << "    ctx.vfpu_ctrl[" << control << "u] = " << psprecomp::hex32(data) << "u;\n";
        break;
    }
    case psprecomp::OpcodeKind::Viim: {
        const std::uint32_t destination = (d.word >> 16u) & 0x7Fu;
        const std::int32_t immediate = static_cast<std::int16_t>(d.word & 0xFFFFu);
        out << "    { const float vfpu_value[1]{static_cast<float>(" << immediate << ")};\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_value, " << destination << "u, 1u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vfim: {
        const std::uint32_t destination = (d.word >> 16u) & 0x7Fu;
        const std::uint32_t half = d.word & 0xFFFFu;
        out << "    { const std::uint16_t vfpu_half = " << half << "u;\n"
            << "      const std::uint32_t vfpu_sign = static_cast<std::uint32_t>(vfpu_half & 0x8000u) << 16u;\n"
            << "      std::uint32_t vfpu_exponent = (vfpu_half >> 10u) & 0x1Fu;\n"
            << "      std::uint32_t vfpu_mantissa = vfpu_half & 0x03FFu;\n"
            << "      std::uint32_t vfpu_bits = 0u;\n"
            << "      if (vfpu_exponent == 0u) {\n"
            << "        if (vfpu_mantissa == 0u) vfpu_bits = vfpu_sign;\n"
            << "        else {\n"
            << "          std::uint32_t shift = 0u;\n"
            << "          while ((vfpu_mantissa & 0x0400u) == 0u) { vfpu_mantissa <<= 1u; ++shift; }\n"
            << "          vfpu_mantissa &= 0x03FFu;\n"
            << "          vfpu_bits = vfpu_sign | ((113u - shift) << 23u) | (vfpu_mantissa << 13u);\n"
            << "        }\n"
            << "      } else if (vfpu_exponent == 31u) {\n"
            << "        vfpu_bits = vfpu_sign | 0x7F800000u | (vfpu_mantissa << 13u);\n"
            << "      } else {\n"
            << "        vfpu_bits = vfpu_sign | ((vfpu_exponent + 112u) << 23u) | (vfpu_mantissa << 13u);\n"
            << "      }\n"
            << "      const float vfpu_value[1]{std::bit_cast<float>(vfpu_bits)};\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_value, " << destination << "u, 1u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vf2h: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vf2h(" << destination << "u, " << source << "u, "
            << length << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vh2f: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t source_length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vh2f(" << destination << "u, " << source << "u, "
            << source_length << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vf2i: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t immediate = (d.word >> 16u) & 31u;
        const std::uint32_t mode = (d.word >> 21u) & 31u;
        out << "    { float vfpu_s[4]{}; std::int32_t vfpu_d[4]{};\n"
            << "      ctx.read_vfpu_vector(vfpu_s, " << source << "u, " << length << "u);\n"
            << "      ctx.apply_vfpu_source_prefix(vfpu_s, " << length << "u, 0u);\n"
            << "      const float vfpu_scale = std::ldexp(1.0f, static_cast<int>(" << immediate << "u));\n"
            << "      for (std::uint32_t vfpu_i = 0; vfpu_i < " << length << "u; ++vfpu_i) {\n"
            << "        if (std::isnan(vfpu_s[vfpu_i])) { vfpu_d[vfpu_i] = std::numeric_limits<std::int32_t>::max(); continue; }\n"
            << "        const double vfpu_scaled = static_cast<double>(vfpu_s[vfpu_i] * vfpu_scale);\n"
            << "        if (vfpu_scaled > static_cast<double>(std::numeric_limits<std::int32_t>::max())) vfpu_d[vfpu_i] = std::numeric_limits<std::int32_t>::max();\n"
            << "        else if (vfpu_scaled <= static_cast<double>(std::numeric_limits<std::int32_t>::min())) vfpu_d[vfpu_i] = std::numeric_limits<std::int32_t>::min();\n"
            << "        else { double vfpu_rounded = 0.0;\n"
            << "          switch (" << mode << "u) {\n"
            << "          case 16u: vfpu_rounded = psprecomp::AllegrexContext::round_ties_to_even(vfpu_scaled); break;\n"
            << "          case 17u: vfpu_rounded = std::trunc(vfpu_scaled); break;\n"
            << "          case 18u: vfpu_rounded = std::ceil(vfpu_scaled); break;\n"
            << "          default: vfpu_rounded = std::floor(vfpu_scaled); break;\n"
            << "          }\n"
            << "          vfpu_d[vfpu_i] = static_cast<std::int32_t>(vfpu_rounded);\n"
            << "        }\n"
            << "      }\n"
            << "      const std::uint32_t vfpu_destination_prefix = ctx.vfpu_ctrl[2];\n"
            << "      for (std::uint32_t vfpu_i = 0; vfpu_i < " << length << "u; ++vfpu_i) {\n"
            << "        if (((vfpu_destination_prefix >> (8u + vfpu_i)) & 1u) == 0u)\n"
            << "          ctx.vfpu[psprecomp::AllegrexContext::vfpu_vector_lane_index(" << destination << "u, " << length << "u, vfpu_i)] = std::bit_cast<float>(static_cast<std::uint32_t>(vfpu_d[vfpu_i]));\n"
            << "      }\n"
            << "      ctx.eat_vfpu_prefixes(); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vi2f: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t immediate = (d.word >> 16u) & 31u;
        out << "    { float vfpu_s[4]{}, vfpu_d[4]{};\n"
            << "      ctx.read_vfpu_vector(vfpu_s, " << source << "u, " << length << "u);\n"
            << "      ctx.apply_vfpu_source_prefix(vfpu_s, " << length << "u, 0u);\n"
            << "      const float vfpu_scale = std::ldexp(1.0f, -static_cast<int>(" << immediate << "u));\n"
            << "      for (std::uint32_t vfpu_i = 0; vfpu_i < " << length << "u; ++vfpu_i) {\n"
            << "        const auto vfpu_integer = static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(vfpu_s[vfpu_i]));\n"
            << "        vfpu_d[vfpu_i] = static_cast<float>(vfpu_integer) * vfpu_scale;\n"
            << "      }\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_d, " << destination << "u, " << length << "u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vx2i: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t source_length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t operation = (d.word >> 16u) & 3u;
        out << "    ctx.execute_vfpu_vx2i(" << destination << "u, " << source << "u, "
            << source_length << "u, " << operation << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Mtv:
        out << "    ctx.set_vfpu_scalar_bits(" << (d.word & 0xFFu) << "u, " << reg(d.rt) << ");\n";
        break;
    case psprecomp::OpcodeKind::Mfv:
        out << "    ctx.set_gpr(" << d.rt << ", ctx.vfpu_scalar_bits(" << (d.word & 0xFFu) << "u));\n";
        break;
    case psprecomp::OpcodeKind::VmidT: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t side = size_code + 1u;
        out << "    ctx.execute_vfpu_matrix_init(" << (d.word & 0x7Fu) << "u, " << side << "u, 3u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vmmov: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t side = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vmmov(" << destination << "u, " << source << "u, "
            << side << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::VfpuMatrixInit: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t side = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t operation = (d.word >> 16u) & 15u;
        out << "    ctx.execute_vfpu_matrix_init(" << destination << "u, " << side << "u, "
            << operation << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vidt: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t offset_mask = length >= 3u ? 3u : 1u;
        const std::uint32_t one_lane = destination & offset_mask;
        out << "    { float vfpu_value[4]{}; vfpu_value[" << one_lane << "u] = 1.0f;\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_value, " << destination
            << "u, " << length << "u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vtfm: {
        const std::uint32_t side = ((d.word >> 23u) & 3u) + 1u;
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t input_length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        out << "    { float vfpu_matrix[16]{}, vfpu_target_raw[4]{}, vfpu_target[4]{}, vfpu_result[4]{};\n"
            << "      ctx.read_vfpu_matrix(vfpu_matrix, " << source << "u, " << side << "u);\n"
            << "      ctx.read_vfpu_vector(vfpu_target_raw, " << target << "u, " << side << "u);\n"
            << "      constexpr std::uint32_t vfpu_side = " << side << "u;\n"
            << "      constexpr std::uint32_t vfpu_input_length = " << input_length << "u;\n"
            << "      for (std::uint32_t i = 0; i < 4u; ++i) vfpu_target[i] = i < vfpu_input_length ? vfpu_target_raw[i] : 0.0f;\n"
            << "      if (vfpu_side - 1u >= vfpu_input_length) vfpu_target[vfpu_side - 1u] = 1.0f;\n"
            << "      for (std::uint32_t row = 0; row + 1u < vfpu_side; ++row) {\n"
            << "        float sum = 0.0f;\n"
            << "        for (std::uint32_t column = 0; column < vfpu_side; ++column) sum += vfpu_matrix[row * 4u + column] * vfpu_target[column];\n"
            << "        vfpu_result[row] = sum;\n"
            << "      }\n"
            << "      float vfpu_final_row[4]{vfpu_matrix[(vfpu_side - 1u) * 4u + 0u], vfpu_matrix[(vfpu_side - 1u) * 4u + 1u],\n"
            << "                              vfpu_matrix[(vfpu_side - 1u) * 4u + 2u], vfpu_matrix[(vfpu_side - 1u) * 4u + 3u]};\n"
            << "      ctx.apply_vfpu_source_prefix(vfpu_final_row, 4u, 0u);\n"
            << "      ctx.apply_vfpu_source_prefix(vfpu_target, 4u, 1u);\n"
            << "      for (std::uint32_t column = 0; column < 4u; ++column) vfpu_result[vfpu_side - 1u] += vfpu_final_row[column] * vfpu_target[column];\n"
            << "      const std::uint32_t vfpu_destination_prefix = ctx.vfpu_ctrl[2];\n"
            << "      const std::uint32_t vfpu_last_lane = vfpu_side - 1u;\n"
            << "      ctx.vfpu_ctrl[2] = ((vfpu_destination_prefix & (1u << 8u)) << vfpu_last_lane) |\n"
            << "                         ((vfpu_destination_prefix & 3u) << (vfpu_last_lane * 2u));\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_result, " << destination << "u, vfpu_side); }\n";
        break;
    }
    case psprecomp::OpcodeKind::VfpuVectorInit: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const bool one = (((d.word >> 16u) & 31u) == 7u);
        out << "    { float vfpu_value[4]{" << (one ? "1.0f, 1.0f, 1.0f, 1.0f" : "") << "};\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_value, " << destination
            << "u, " << length << "u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vmmul: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t side = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        out << "    { float vfpu_s[16]{}, vfpu_t[16]{}, vfpu_d[16]{};\n"
            << "      ctx.read_vfpu_matrix(vfpu_s, " << source << "u, " << side << "u);\n"
            << "      ctx.read_vfpu_matrix(vfpu_t, " << target << "u, " << side << "u);\n"
            << "      for (std::uint32_t a = 0; a < " << side << "u; ++a) {\n"
            << "        for (std::uint32_t b = 0; b < " << side << "u; ++b) {\n"
            << "          float sum = 0.0f;\n"
            << "          for (std::uint32_t c = 0; c < " << side << "u; ++c) sum += vfpu_s[b * 4u + c] * vfpu_t[a * 4u + c];\n"
            << "          vfpu_d[a * 4u + b] = sum;\n"
            << "        }\n"
            << "      }\n"
            << "      ctx.write_vfpu_matrix(vfpu_d, " << destination << "u, " << side << "u);\n"
            << "      ctx.eat_vfpu_prefixes(); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vmscl: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t side = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vmscl(" << destination << "u, " << source << "u, "
            << target << "u, " << side << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vrot: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t immediate = (d.word >> 16u) & 31u;
        out << "    ctx.execute_vfpu_vrot(" << destination << "u, " << source << "u, "
            << length << "u, " << immediate << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vocp: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vocp(" << destination << "u, " << source << "u, "
            << length << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::VfpuHorizontal: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const bool average = ((d.word >> 16u) & 31u) == 7u;
        out << "    ctx.execute_vfpu_horizontal(" << destination << "u, " << source << "u, "
            << length << "u, " << (average ? "true" : "false") << ");\n";
        break;
    }
    case psprecomp::OpcodeKind::VfpuVec3: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        const std::uint32_t major = d.word >> 26u;
        const std::uint32_t operation = major == 0x19u ? 2u : ((d.word >> 23u) & 7u);
        const char *expression = operation == 0u ? "vfpu_s[i] + vfpu_t[i]"
                               : operation == 1u ? "vfpu_s[i] - vfpu_t[i]"
                               : operation == 2u ? "vfpu_s[i] * vfpu_t[i]"
                                                 : "vfpu_s[i] / vfpu_t[i]";
        out << "    { float vfpu_s[4]{}, vfpu_t[4]{}, vfpu_d[4]{};\n"
            << "      ctx.read_vfpu_vector_with_source_prefix(vfpu_s, " << source << "u, " << length << "u, 0u);\n"
            << "      ctx.read_vfpu_vector_with_source_prefix(vfpu_t, " << target << "u, " << length << "u, 1u);\n"
            << "      for (std::uint32_t i = 0; i < " << length << "u; ++i) vfpu_d[i] = " << expression << ";\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_d, " << destination << "u, " << length << "u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vdot: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vdot(" << destination << "u, " << source << "u, "
            << target << "u, " << length << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vhdp: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vhdp(" << destination << "u, " << source << "u, "
            << target << "u, " << length << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::VcrossQuat: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        out << "    ctx.execute_vfpu_cross_quat(" << destination << "u, " << source << "u, "
            << target << "u, " << length << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vminmax: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        const bool maximum = ((d.word >> 23u) & 7u) == 3u;
        out << "    ctx.execute_vfpu_vminmax(" << destination << "u, " << source << "u, "
            << target << "u, " << length << "u, " << (maximum ? "true" : "false") << ");\n";
        break;
    }
    case psprecomp::OpcodeKind::VfpuCompare3: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        const std::uint32_t operation = (d.word >> 23u) & 7u;
        out << "    ctx.execute_vfpu_compare3(" << destination << "u, " << source << "u, "
            << target << "u, " << length << "u, " << operation << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vcmp: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        const std::uint32_t condition = d.word & 15u;
        out << "    ctx.execute_vfpu_vcmp(" << source << "u, " << target << "u, "
            << length << "u, " << condition << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vcmov: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t condition_index = (d.word >> 16u) & 7u;
        const bool move_if_false = ((d.word >> 19u) & 1u) != 0u;
        out << "    ctx.execute_vfpu_vcmov(" << destination << "u, " << source << "u, "
            << length << "u, " << condition_index << "u, "
            << (move_if_false ? "true" : "false") << ");\n";
        break;
    }
    case psprecomp::OpcodeKind::Vscl: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vscl(" << destination << "u, " << source << "u, "
            << target << "u, " << length << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::VfpuUnary: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t operation = (d.word >> 16u) & 31u;
        std::string expression;
        switch (operation) {
        case 0u: expression = "vfpu_s[i]"; break;
        case 1u: expression = "std::fabs(vfpu_s[i])"; break;
        case 2u: expression = "-vfpu_s[i]"; break;
        case 4u: expression = "vfpu_s[i] <= 0.0f ? 0.0f : (vfpu_s[i] > 1.0f ? 1.0f : vfpu_s[i])"; break;
        case 5u: expression = "vfpu_s[i] < -1.0f ? -1.0f : (vfpu_s[i] > 1.0f ? 1.0f : vfpu_s[i])"; break;
        case 16u: expression = "1.0f / vfpu_s[i]"; break;
        case 17u: expression = "1.0f / std::sqrt(vfpu_s[i])"; break;
        case 18u: expression = "std::sin(vfpu_s[i] * 1.57079632679489661923f)"; break;
        case 19u: expression = "std::cos(vfpu_s[i] * 1.57079632679489661923f)"; break;
        case 20u: expression = "std::exp2(vfpu_s[i])"; break;
        case 21u: expression = "std::log2(vfpu_s[i])"; break;
        case 22u: expression = "std::fabs(std::sqrt(vfpu_s[i]))"; break;
        case 23u: expression = "std::asin(vfpu_s[i]) * 0.63661977236758134308f"; break;
        case 24u: expression = "-1.0f / vfpu_s[i]"; break;
        case 26u: expression = "-std::sin(vfpu_s[i] * 1.57079632679489661923f)"; break;
        default: expression = "1.0f / std::exp2(vfpu_s[i])"; break;
        }
        out << "    { float vfpu_s[4]{}, vfpu_d[4]{};\n"
            << "      ctx.read_vfpu_vector_with_source_prefix(vfpu_s, " << source << "u, " << length << "u, 0u);\n"
            << "      for (std::uint32_t i = 0; i < " << length << "u; ++i) vfpu_d[i] = " << expression << ";\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_d, " << destination << "u, " << length << "u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vcst: {
        static constexpr std::uint32_t constant_bits[32] = {
            0x00000000u, 0x7F7FFFFFu, 0x3FB504F3u, 0x3F3504F3u,
            0x3F906EBAu, 0x3F22F983u, 0x3EA2F983u, 0x3F490FDBu,
            0x3FC90FDBu, 0x40490FDBu, 0x402DF854u, 0x3FB8AA3Bu,
            0x3EDE5BD9u, 0x3F317218u, 0x40135D8Eu, 0x40C90FDBu,
            0x3F060A92u, 0x3E9A209Bu, 0x40549A78u, 0x3F5DB3D7u,
            0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        };
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t selector = (d.word >> 16u) & 31u;
        const std::uint32_t destination = d.word & 0x7Fu;
        out << "    { const float vfpu_constant = std::bit_cast<float>("
            << psprecomp::hex32(constant_bits[selector]) << "u);\n"
            << "      const float vfpu_value[4]{vfpu_constant, vfpu_constant, vfpu_constant, vfpu_constant};\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_value, " << destination
            << "u, " << length << "u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Lvs: {
        const std::int32_t offset = static_cast<std::int16_t>(d.word & 0xFFFCu);
        const std::uint32_t scalar_register = ((d.word >> 16u) & 0x1Fu) | ((d.word & 3u) << 5u);
        out << "    ctx.set_vfpu_scalar_bits(" << scalar_register << "u, mem.aot_load32("
            << reg(d.rs) << " + static_cast<std::uint32_t>(" << offset << ")));\n";
        break;
    }
    case psprecomp::OpcodeKind::Svs: {
        const std::int32_t offset = static_cast<std::int16_t>(d.word & 0xFFFCu);
        const std::uint32_t scalar_register = ((d.word >> 16u) & 0x1Fu) | ((d.word & 3u) << 5u);
        out << "    mem.aot_store32(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << offset
            << "), ctx.vfpu_scalar_bits(" << scalar_register << "u));\n";
        break;
    }
    case psprecomp::OpcodeKind::Lvq: {
        const std::int32_t offset = static_cast<std::int16_t>(d.word & 0xFFFCu);
        const std::uint32_t vector_register = ((d.word >> 16u) & 0x1Fu) | ((d.word & 1u) << 5u);
        out << "    { const std::uint32_t vfpu_address = " << reg(d.rs) << " + static_cast<std::uint32_t>(" << offset << ");\n"
            << "      float vfpu_value[4]{\n"
            << "        std::bit_cast<float>(mem.aot_load32(vfpu_address + 0u)),\n"
            << "        std::bit_cast<float>(mem.aot_load32(vfpu_address + 4u)),\n"
            << "        std::bit_cast<float>(mem.aot_load32(vfpu_address + 8u)),\n"
            << "        std::bit_cast<float>(mem.aot_load32(vfpu_address + 12u))};\n"
            << "      ctx.write_vfpu_vector(vfpu_value, " << vector_register << "u, 4u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Svq: {
        const std::int32_t offset = static_cast<std::int16_t>(d.word & 0xFFFCu);
        const std::uint32_t vector_register = ((d.word >> 16u) & 0x1Fu) | ((d.word & 1u) << 5u);
        out << "    { float vfpu_value[4]{}; ctx.read_vfpu_vector(vfpu_value, " << vector_register << "u, 4u);\n"
            << "      const std::uint32_t vfpu_address = " << reg(d.rs) << " + static_cast<std::uint32_t>(" << offset << ");\n"
            << "      mem.aot_store32(vfpu_address + 0u, std::bit_cast<std::uint32_t>(vfpu_value[0]));\n"
            << "      mem.aot_store32(vfpu_address + 4u, std::bit_cast<std::uint32_t>(vfpu_value[1]));\n"
            << "      mem.aot_store32(vfpu_address + 8u, std::bit_cast<std::uint32_t>(vfpu_value[2]));\n"
            << "      mem.aot_store32(vfpu_address + 12u, std::bit_cast<std::uint32_t>(vfpu_value[3])); }\n";
        break;
    }
    default:
        out << "    rt.unsupported(" << psprecomp::hex32(pc) << "u, " << psprecomp::hex32(d.word)
            << "u, \"" << cpp_escape(d.mnemonic) << " not lowered yet\"); return;\n";
        break;
    }
    return out.str();
}


} // namespace mhf::psp::recomp
