// Runner for a recompiled homebrew program (our own PSP test programs, docs/10-roadmap.md phase 1).
//
// usage: <runner> <program.elf> [--trace] [--memstick <dir>]
// The ELF supplies the data sections; the code is the recompiled C++ linked into this executable.

#include "mhf/psp/generated.hpp"
#include "mhf/psp/runtime.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#ifndef MHF_PROGRAM
#error "MHF_PROGRAM must name the recompiled program"
#endif
#define MHF_CONCAT2(a, b) a##b
#define MHF_CONCAT(a, b) MHF_CONCAT2(a, b)
#define MHF_STRINGIFY2(a) #a
#define MHF_STRINGIFY(a) MHF_STRINGIFY2(a)

namespace mhf::psp::generated {
const Program &MHF_CONCAT(MHF_PROGRAM, _program)();
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <program.elf> [--trace] [--memstick <dir>]\n", argv[0]);
        return 2;
    }
    mhf::psp::RuntimeOptions options;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--trace") == 0) {
            options.trace_imports = true;
        } else if (std::strcmp(argv[i], "--memstick") == 0 && i + 1 < argc) {
            options.memstick_root = argv[++i];
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    try {
        mhf::psp::Runtime runtime{options};
        runtime.load_elf(argv[1]);
        const auto &program = mhf::psp::generated::MHF_CONCAT(MHF_PROGRAM, _program)();
        const std::string boot = std::string("ms0:/PSP/GAME/") + MHF_STRINGIFY(MHF_PROGRAM) + "/EBOOT.PBP";
        return runtime.run(program, boot);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
