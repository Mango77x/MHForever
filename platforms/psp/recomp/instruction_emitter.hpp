#pragma once

// Translation of one non-control-flow Allegrex instruction into C++ statements.
//
// The emitted code expects these names in scope: `rt` (mhf::psp::Runtime&), `ctx` (psprecomp::AllegrexContext&) and
// `mem` (psprecomp::GuestMemory&). Control flow (branches, jumps, calls) is handled by the function emitter.

#include "psprecomp/decoder.hpp"

#include <cstdint>
#include <string>

namespace mhf::psp::recomp {

// C++ expression for a guest GPR (literal 0u for $zero).
[[nodiscard]] std::string reg(std::uint32_t index);

[[nodiscard]] std::string cpp_escape(const std::string &text);

// Allegrex madd/maddu/msub/msubu (not classified by the vendored decoder).
[[nodiscard]] bool is_multiply_accumulate(std::uint32_t word);

// True when control cannot continue past `d` (undecodable instruction or a guest trap).
[[nodiscard]] bool ends_block(const psprecomp::DecodedInstruction &d);

// Statements implementing `d` located at `pc`. Unsupported instructions emit a call to rt.unsupported().
[[nodiscard]] std::string emit_instruction(const psprecomp::DecodedInstruction &d, std::uint32_t pc);

} // namespace mhf::psp::recomp
