// Freedom Unite host: runs the recompiled game against the player's own ISO (docs/06-mhfu-module.md).
//
// usage: mhfu --iso <image.iso> --exe <decrypted executable> [--trace] [--memstick <dir>]
//             [--window] [--dump-frames <dir> [--dump-every N] [--dump-from S]] [--seconds S] [--quiet-overlays]
//             [--input "<seconds>:<button>,..."] [--name <text>] [--software] [--scale N]
// The executable supplies the initial data sections during development (open question Q-16 for releases).

#include "mhf/psp/frame_dump.hpp"
#include "mhf/psp/generated.hpp"
#include "mhf/psp/host.hpp"
#include "mhf/psp/runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>

#if defined(MHF_HAS_FRONTEND)
#include "psp_frontend.hpp"
#endif
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace mhf::psp::generated {
std::span<const Program *const> mhfu_programs();
}

namespace {

using mhf::psp::Context;
using mhf::psp::Runtime;

bool g_log_overlays = true;

// Scripted input for unattended runs: "12.5:cross,15:start" presses each button for 150 ms at that time.
struct ScriptedPress {
    double at_seconds;
    std::uint32_t buttons;
};

std::vector<ScriptedPress> parse_input_script(const std::string &text) {
    static const std::pair<const char *, std::uint32_t> names[] = {
        {"cross", mhf::psp::buttons::kCross}, {"circle", mhf::psp::buttons::kCircle},
        {"square", mhf::psp::buttons::kSquare}, {"triangle", mhf::psp::buttons::kTriangle},
        {"start", mhf::psp::buttons::kStart}, {"select", mhf::psp::buttons::kSelect},
        {"up", mhf::psp::buttons::kUp}, {"down", mhf::psp::buttons::kDown},
        {"left", mhf::psp::buttons::kLeft}, {"right", mhf::psp::buttons::kRight},
        {"l", mhf::psp::buttons::kLTrigger}, {"r", mhf::psp::buttons::kRTrigger},
    };
    std::vector<ScriptedPress> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto comma = text.find(',', start);
        const std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const auto colon = item.find(':');
        if (colon != std::string::npos) {
            const std::string name = item.substr(colon + 1);
            for (const auto &[n, bit] : names) {
                if (name == n) out.push_back({std::stod(item.substr(0, colon)), bit});
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

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
    double dump_from = 0.0; // seconds of run time before frames are dumped
    double seconds = 0;
    bool window = false;
    bool software = false; // --software: draw with the CPU renderer even in a window
    int render_scale = 4;  // --scale N: internal resolution = N x 480x272 (GPU renderer)
    std::vector<ScriptedPress> script;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--iso" && i + 1 < argc) options.disc_image = argv[++i];
        else if (a == "--exe" && i + 1 < argc) exe = argv[++i];
        else if (a == "--memstick" && i + 1 < argc) options.memstick_root = argv[++i];
        else if (a == "--trace") options.trace_imports = true;
        else if (a == "--quiet-overlays") g_log_overlays = false;
        else if (a == "--dump-frames" && i + 1 < argc) dump_folder = argv[++i];
        else if (a == "--dump-every" && i + 1 < argc) dump_every = static_cast<unsigned>(std::stoul(argv[++i]));
        else if (a == "--dump-from" && i + 1 < argc) dump_from = std::stod(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::stod(argv[++i]);
        else if (a == "--window") window = true;
        else if (a == "--software") software = true;
        else if (a == "--scale" && i + 1 < argc) render_scale = std::clamp(std::stoi(argv[++i]), 1, 8);
        else if (a == "--input" && i + 1 < argc) script = parse_input_script(argv[++i]);
        else if (a == "--name" && i + 1 < argc) options.osk_default_text = argv[++i]; // text typed into keyboards
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
        std::function<void()> extra;
        if (!dump_folder.empty() || seconds > 0 || !script.empty()) {
            if (!dump_folder.empty()) std::filesystem::create_directories(dump_folder);
            auto *rt = &runtime;
            std::uint64_t last_vblank = 0;
            unsigned frame = 0;
            extra = [=]() mutable {
                const std::uint64_t now = rt->now_us();
                if (seconds > 0 && static_cast<double>(now) > seconds * 1e6) {
                    std::fprintf(stderr, "[mhfu] time limit reached\n");
                    rt->dump_threads();
                    rt->exit_program_from_host(0);
                }
                if (!script.empty()) {
                    mhf::psp::ControllerState pad;
                    for (const auto &p : script) {
                        const double t = static_cast<double>(now) / 1e6;
                        if (t >= p.at_seconds && t < p.at_seconds + 0.15) pad.buttons |= p.buttons;
                    }
                    mhf::psp::set_controller_state(*rt, pad);
                }
                // One dump per `dump_every` vblanks, even when the host is not polled on that exact vblank.
                const std::uint64_t slot = now / mhf::psp::kVblankPeriodUs / dump_every;
                if (dump_folder.empty() || slot == last_vblank || static_cast<double>(now) < dump_from * 1e6) return;
                last_vblank = slot;
                char name[32];
                std::snprintf(name, sizeof name, "frame_%06u.png", ++frame);
                mhf::psp::dump_display(*rt, dump_folder / name);
            };
        }
#if defined(MHF_HAS_FRONTEND)
        mhf::psp::Frontend frontend;
        if (window && frontend.open("MH-Forever", false, 2, !software, render_scale)) {
            frontend.attach(runtime, extra);
        } else if (extra) {
            runtime.kernel_host_poll(extra);
        }
#else
        if (window) std::fprintf(stderr, "[mhfu] built without the SDL frontend; running headless\n");
        if (extra) runtime.kernel_host_poll(extra);
#endif
        return runtime.run(*programs[0], "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
