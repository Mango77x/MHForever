#pragma once

// Function discovery for the function-level recompiler (docs/04-recompiler.md §3).
//
// Model: one C++ function per guest function. Calls (`jal`, `jalr`, link branches) become C++ calls that return to the
// instruction after the delay slot; `jr $ra` becomes `return`. A function's body is everything reachable from its entry
// through branches, intra-function jumps and jump tables. A block reachable from two functions is emitted in both, which
// is harmless because instructions have no identity of their own.

#include "psprecomp/decoder.hpp"
#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mhf::psp::recomp {

struct CodeRange {
    std::uint32_t start{};
    std::uint32_t end{}; // exclusive
};

struct ImportStub {
    std::string library;
    std::uint32_t nid{};
    std::uint32_t address{};
};

struct JumpTable {
    std::uint32_t jump_pc{};       // address of the `jr`
    std::uint32_t table_address{};
    std::vector<std::uint32_t> targets;
};

struct Function {
    std::uint32_t entry{};
    std::set<std::uint32_t> instructions; // addresses of every instruction in the body (delay slots included)
    std::set<std::uint32_t> labels;       // block starts: entry, branch targets, call continuations, table targets
    std::map<std::uint32_t, JumpTable> jump_tables;
    bool has_unresolved_jump{};           // a `jr` that is neither a return nor a recognized table
};

struct ProgramInput {
    const psprecomp::GuestMemory *memory{};
    std::vector<CodeRange> code;               // executable ranges (import stubs excluded)
    std::vector<CodeRange> data;               // ranges scanned for code pointers
    std::vector<ImportStub> imports;
    std::vector<std::uint32_t> extra_entries;  // entry point, exports, known callbacks
};

struct Analysis {
    std::map<std::uint32_t, Function> functions; // by entry
    std::map<std::uint32_t, std::size_t> import_by_stub;
    std::size_t data_pointer_seeds{};
    std::size_t constant_pointer_seeds{};
    std::size_t jump_tables{};
    std::size_t unresolved_jumps{};
};

// `b` and friends: conditional encodings whose condition is always true (no fall-through path).
[[nodiscard]] bool is_unconditional_branch(const psprecomp::DecodedInstruction &d);

[[nodiscard]] bool in_ranges(const std::vector<CodeRange> &ranges, std::uint32_t address) noexcept;

[[nodiscard]] Analysis analyze(const ProgramInput &input);

} // namespace mhf::psp::recomp
