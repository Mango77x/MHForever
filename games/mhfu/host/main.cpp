// Freedom Unite host: runs the recompiled game against the player's own ISO (docs/06-mhfu-module.md).
//
// usage: mhfu --iso <image.iso> --exe <decrypted executable> [--trace] [--memstick <dir>]
//             [--dump-frames <dir> [--dump-every N]] [--seconds S] [--quiet-overlays]
// The executable supplies the initial data sections during development (open question Q-16 for releases).

#include "mhf/psp/frame_dump.hpp"
#include "mhf/psp/generated.hpp"
#include "mhf/psp/host.hpp"
#include "mhf/psp/runtime.hpp"

#include <cstdio>
#include <filesystem>
#include <cstring>
#include <exception>
#include <string>

namespace mhf::psp::generated {
std::span<const Program *const> mhfu_programs();
}

namespace {

using mhf::psp::Context;
using mhf::psp::Runtime;

bool g_log_overlays = true;

// Hook on mh_overlay_prepare(image, size): the overlay image (header included) is already in guest memory and its
// static initializers are about to run, so its recompiled code must become the active one for its region now.
void on_overlay_prepare(Runtime &rt, Context &ctx) {
    const std::uint32_t image = ctx.gpr[4];
    const auto overlay_id = static_cast<std::int32_t>(rt.memory().load32(image + 4));
    const bool ok = rt.activate_overlay(overlay_id);
    if (g_log_overlays || !ok) {
        std::fprintf(stderr, "[mhfu] overlay %d %s at %08X\n", overlay_id, ok ? "activated" : "UNKNOWN", image);
    }
}

} // namespace

int main(int argc, char **argv) {
    mhf::psp::RuntimeOptions options;
    std::string exe;
    std::filesystem::path dump_folder;
    unsigned dump_every = 30;
    double seconds = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--iso" && i + 1 < argc) options.disc_image = argv[++i];
        else if (a == "--exe" && i + 1 < argc) exe = argv[++i];
        else if (a == "--memstick" && i + 1 < argc) options.memstick_root = argv[++i];
        else if (a == "--trace") options.trace_imports = true;
        else if (a == "--quiet-overlays") g_log_overlays = false;
        else if (a == "--dump-frames" && i + 1 < argc) dump_folder = argv[++i];
        else if (a == "--dump-every" && i + 1 < argc) dump_every = static_cast<unsigned>(std::stoul(argv[++i]));
        else if (a == "--seconds" && i + 1 < argc) seconds = std::stod(argv[++i]);
        else {
            std::fprintf(stderr, "usage: %s --iso <image.iso> --exe <decrypted executable> [--trace] [--memstick <dir>]\n",
                         argv[0]);
            return 2;
        }
    }
    if (options.disc_image.empty() || exe.empty()) {
        std::fprintf(stderr, "error: --iso and --exe are required\n");
        return 2;
    }
    try {
        Runtime runtime{options};
        runtime.load_elf(exe);
        const auto programs = mhf::psp::generated::mhfu_programs();
        for (std::size_t i = 1; i < programs.size(); ++i) {
            runtime.add_overlay(*programs[i]);
        }
        runtime.register_hook("mh_overlay_prepare", &on_overlay_prepare);
        if (!dump_folder.empty() || seconds > 0) {
            if (!dump_folder.empty()) std::filesystem::create_directories(dump_folder);
            auto *rt = &runtime;
            std::uint64_t last_vblank = 0;
            unsigned frame = 0;
            runtime.kernel_host_poll([=]() mutable {
                const std::uint64_t now = rt->now_us();
                if (seconds > 0 && static_cast<double>(now) > seconds * 1e6) {
                    std::fprintf(stderr, "[mhfu] time limit reached\n");
                    rt->exit_program_from_host(0);
                }
                const std::uint64_t vblank = now / mhf::psp::kVblankPeriodUs;
                if (dump_folder.empty() || vblank == last_vblank) return;
                last_vblank = vblank;
                if (vblank % dump_every != 0) return;
                char name[32];
                std::snprintf(name, sizeof name, "frame_%06u.png", ++frame);
                mhf::psp::dump_display(*rt, dump_folder / name);
            });
        }
        return runtime.run(*programs[0], "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
