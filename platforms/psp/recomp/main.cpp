// mhf_recomp — function-level Allegrex → C++ recompiler (docs/04-recompiler.md).
//
// Input: a decrypted PSP ELF (our own homebrew, or the developer's own game executable). Output: C++ sources in a
// git-ignored folder. Generated code from a game must never be committed (see CONTRIBUTING.md).

#include "analysis.hpp"
#include "emitter.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/nid_registry.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace mhf::psp::recomp;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: mhf_recomp --elf <file.elf> --out <dir> --name <identifier> [--parts N] [--nids nids.csv]\n"
                 "                  [--list-imports]\n");
}

constexpr std::uint32_t kShfAlloc = 0x2u;
constexpr std::uint32_t kShfExec = 0x4u;
constexpr std::uint32_t kShtNobits = 8u;

} // namespace

int main(int argc, char **argv) {
    fs::path elf_path, out_dir, nids_path;
    std::string name;
    std::size_t parts = 8;
    bool list_imports = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--elf") elf_path = value();
        else if (arg == "--out") out_dir = value();
        else if (arg == "--name") name = value();
        else if (arg == "--parts") parts = std::stoul(value());
        else if (arg == "--nids") nids_path = value();
        else if (arg == "--list-imports") list_imports = true;
        else {
            usage();
            return 2;
        }
    }
    if (elf_path.empty() || (!list_imports && (out_dir.empty() || name.empty()))) {
        usage();
        return 2;
    }

    try {
        const auto elf = psprecomp::Elf32Image::from_file(elf_path);
        if (elf.is_psp_prx()) {
            std::fprintf(stderr, "error: relocatable PRX modules are not supported yet; build a static ELF\n");
            return 1;
        }
        psprecomp::GuestMemory memory;
        elf.load_into(memory, 0u);
        const auto module = elf.find_module_info(memory, 0u);
        if (!module) {
            std::fprintf(stderr, "error: no sceModuleInfo found\n");
            return 1;
        }

        ProgramInput input;
        input.memory = &memory;
        for (const auto &s : elf.sections()) {
            if ((s.flags & kShfAlloc) == 0 || s.size == 0 || s.address == 0) {
                continue;
            }
            if ((s.flags & kShfExec) != 0) {
                if (s.name != ".sceStub.text") {
                    input.code.push_back({s.address, s.address + s.size});
                }
            } else if (s.type != kShtNobits) {
                input.data.push_back({s.address, s.address + s.size});
            }
        }
        for (const auto &imp : elf.scan_imports(memory, *module)) {
            input.imports.push_back({imp.library, imp.nid, imp.stub_address});
        }
        input.extra_entries.push_back(elf.entry());

        if (list_imports) {
            psprecomp::NidRegistry nids;
            if (!nids_path.empty()) {
                nids.load_csv(nids_path);
            }
            for (const auto &imp : input.imports) {
                const auto resolved = nids.resolve(imp.library, imp.nid);
                std::printf("%s %s %s\n", imp.library.c_str(), psprecomp::hex32(imp.nid).c_str(),
                            resolved ? resolved->c_str() : "?");
            }
            return 0;
        }

        const auto analysis = analyze(input);
        EmitOptions options;
        options.program_name = name;
        options.entry = elf.entry();
        options.gp = module->gp;
        options.parts = parts;
        const auto result = emit_program(input, analysis, options, out_dir);
        std::printf("%s: %zu functions, %zu instructions, %zu imports, %zu jump tables, %zu unresolved jumps, "
                    "%zu data-pointer seeds, %zu constant-pointer seeds\n",
                    name.c_str(), result.functions, result.instructions, input.imports.size(), analysis.jump_tables,
                    analysis.unresolved_jumps, analysis.data_pointer_seeds, analysis.constant_pointer_seeds);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
