#pragma once

// Writes the C++ for an analyzed program (docs/04-recompiler.md §5).

#include "analysis.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mhf::psp::recomp {

struct EmitOptions {
    std::string program_name;     // C++ identifier; generated code lives in namespace mhf_gen::<program_name>
    std::uint32_t entry{};        // module entry point
    std::uint32_t gp{};           // module $gp
    std::size_t parts{8};         // number of function source files (fixed so build systems can list them)
};

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
