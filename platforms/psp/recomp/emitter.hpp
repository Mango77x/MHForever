#pragma once

// Writes the C++ for an analyzed program (docs/04-recompiler.md §5).

#include "analysis.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mhf::psp::recomp {

struct EmitOptions {
    std::string program_name;     // C++ identifier; generated code lives in namespace mhf_gen::<program_name>
    std::uint32_t entry{};        // module entry point
    std::uint32_t gp{};           // module $gp
    std::size_t parts{8};         // number of function source files (fixed so build systems can list them)
    std::uint32_t code_start{};   // address range served by this module's function table
    std::uint32_t code_end{};
    std::int32_t overlay_id{-1};  // >= 0 for overlays (the id in their header)
};

// Writes <out>/<registry_name>_programs.cpp defining `std::span<const Program *const> <registry_name>_programs()` over the
// given program names (main module first, then overlays).
void emit_registry(const std::string &registry_name, const std::vector<std::string> &program_names,
                   const std::filesystem::path &out_dir);

// Region-map [data] entries by name: the address in this release, or std::nullopt when only other releases map it.
using RegionData = std::map<std::string, std::optional<std::uint32_t>>;

// The [data] entries of the region map `symbols` (games/<id>/regions/<disc>/symbols.toml), plus every [data] name that
// the other region maps next to it list, without an address: host code that names an entry builds for every release.
RegionData region_data_entries(const std::filesystem::path &symbols);

// Writes <out>/<program_name>_region_data.hpp: namespace mhf::psp::generated::<program_name>_region_data with one
// `inline constexpr std::optional<std::uint32_t>` per entry. Host code names the entries directly, so a misspelled
// name fails the build and every release builds the same code. Returns the path of the file.
std::filesystem::path emit_region_data(const std::string &program_name, const RegionData &entries,
                                       const std::filesystem::path &out_dir);

// One [patches] entry of a region map: `name = "<module> <address> immediate <value>"` or `"<module> <address> never"`,
// where <module> is "executable" or an overlay's file name ("lobby_task.ovl"). Overlays share addresses, so the module
// says which one the address is in.
struct RegionPatch {
    std::string module;
    std::uint32_t address{};
    InstructionPatch patch;
};

// The [patches] entries of the region map `symbols`. Throws std::invalid_argument on an entry it cannot read.
std::vector<RegionPatch> region_patches(const std::filesystem::path &symbols);

// One [guards] entry of a region map: `name = "<module> 0x<address> [0x<address> ...]"`, words the game may rewrite
// while it runs (a patch loaded at run time, such as FUComplete's). <module> as for [patches].
struct RegionGuard {
    std::string module;
    std::vector<std::uint32_t> addresses;
};

// The [guards] entries of the region map `symbols`. Throws std::invalid_argument on an entry it cannot read.
std::vector<RegionGuard> region_guards(const std::filesystem::path &symbols);

// Empty when `patch` can be applied to `d`; otherwise why not. Immediate: an ALU instruction with an immediate operand
// (addiu, slti, sltiu, andi, ori, xori). Never: a conditional branch that does not link and is not always taken.
std::string patch_mismatch(const psprecomp::DecodedInstruction &d, const InstructionPatch &patch);

struct EmitResult {
    std::vector<std::filesystem::path> files;
    std::size_t functions{};
    std::size_t instructions{};
};

// Writes <out>/<name>.hpp, <out>/<name>_part<N>.cpp (N < parts) and <out>/<name>_table.cpp. Files whose content did
// not change are left untouched so incremental builds stay fast.
EmitResult emit_program(const ProgramInput &input, const Analysis &analysis, const EmitOptions &options,
                        const std::filesystem::path &out_dir);

} // namespace mhf::psp::recomp
