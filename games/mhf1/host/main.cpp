// Monster Hunter Freedom (PSP) on the MHForever PSP platform (docs/16-mhf1-module.md).
//
// The game program: loads the developer's prepared executable, installs the recompiled code (the executable's and its
// overlays'), connects the hooks its region map names and runs the game, in a window (platforms/psp/frontend) or
// headless for tests.
//
// Usage: mhf1 --iso <image.iso> --exe <prepared executable> [--memstick <dir>] [--window] [--fullscreen] [--scale N]
//             [--fps 30..180] [--seconds N] [--dump-frames <dir> [--dump-every N] [--dump-from S]]
//             [--input "<seconds>:<button>,..."] [--name <text>] [--auto-dialogs] [--trace] [--quiet-overlays]

#include "mhf/psp/abi.hpp"
#include "mhf/psp/frame_dump.hpp"
#include "mhf/psp/generated.hpp"
#include "mhf/psp/host.hpp"
#include "mhf/psp/runtime.hpp"
#include "iso9660.hpp"
#include "param_sfo.hpp"

#include "host/keymap.hpp"
#include "host/padmap.hpp"

#if !__has_include("mhf1_region_data.hpp")
#error "mhf1_region_data.hpp is missing from MHF_MHF1_GENERATED: re-run games/mhf1/tools/prepare.py"
#endif
#include "mhf1_region_data.hpp" // generated from this release's regions/<disc>/symbols.toml [data]

#if defined(MHF_HAS_FRONTEND)
#include "psp_frontend.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mhf::psp::generated {
std::span<const Program *const> mhf1_programs(); // the recompiled programs: the executable's, then its overlays'
}

namespace {

using mhf::psp::Context;
using mhf::psp::Runtime;
namespace region = mhf::psp::generated::mhf1_region_data;

bool g_log_overlays = true;

// Hook on mh_overlay_prepare(image, size): the overlay image (header included) is in guest memory and its static
// initializers are about to run, so its recompiled code becomes the active one for its region now. The overlay's id is
// the header's second word, as in Freedom Unite.
void on_overlay_prepare(Runtime &rt, Context &ctx) {
    const std::uint32_t image = ctx.gpr[4];
    const auto overlay_id = static_cast<std::int32_t>(rt.memory().load32(image + 4));
    const bool ok = rt.activate_overlay(overlay_id);
    if (g_log_overlays || !ok) {
        std::fprintf(stderr, "[mhf1] overlay %d %s at %08X\n", overlay_id, ok ? "activated" : "UNKNOWN", image);
    }
}

// The game's wait for its draw-done flag (region map mh_draw_wait): a loop that reads the flag until an interrupt (a
// finished display list) sets it. Hooked at its start, the host sleeps until an interrupt may be due instead of keeping
// a core busy, services it and looks again; the game's own loop then finds the flag set and returns.
constexpr bool kDrawWaitMapped = region::mh_draw_done.has_value();
void on_draw_wait([[maybe_unused]] Runtime &rt, Context &) {
    if constexpr (kDrawWaitMapped) {
        while (rt.memory().load8(*region::mh_draw_done) == 0) {
            mhf::psp::abi::wait_for_interrupt();
            mhf::psp::abi::service_interrupts(rt);
        }
    }
}

// Scripted input for unattended runs: "12.5:cross,15:start" presses each button for 150 ms at that time; "20:sup/3"
// holds the analog stick up (sup, sdown, sleft, sright) or a button for 3 seconds.
struct ScriptedPress {
    double at_seconds;
    std::uint32_t buttons;
    double duration = 0.15;
    int stick_x = 0, stick_y = 0;
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
        if (const auto colon = item.find(':'); colon != std::string::npos) {
            std::string name = item.substr(colon + 1);
            double duration = 0.15;
            if (const auto slash = name.find('/'); slash != std::string::npos) {
                duration = std::stod(name.substr(slash + 1));
                name.resize(slash);
            }
            const double at = std::stod(item.substr(0, colon));
            for (const auto &[n, bit] : names) {
                if (name == n) out.push_back({at, bit, duration});
            }
            if (name == "sup") out.push_back({at, 0, duration, 0, -127});
            if (name == "sdown") out.push_back({at, 0, duration, 0, 127});
            if (name == "sleft") out.push_back({at, 0, duration, -127, 0});
            if (name == "sright") out.push_back({at, 0, duration, 127, 0});
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// The window's title: the disc's own (PARAM.SFO), without its trademark sign.
std::string window_title(const std::filesystem::path &disc_image) {
    std::string title;
    try {
        mhf::psp::IsoImage image(disc_image);
        if (const auto entry = image.find("/PSP_GAME/PARAM.SFO")) {
            if (const auto sfo = mhf::psp::ParamSfo::parse(image.read_file(*entry, 0, entry->size))) title = sfo->string("TITLE");
        }
    } catch (const std::exception &) {
    }
    for (const std::string mark : {"(TM)", "\xE2\x84\xA2", "\xC2\xAE"}) {
        for (auto at = title.find(mark); at != std::string::npos; at = title.find(mark)) title.erase(at, mark.size());
    }
    return title.empty() ? std::string("MHForever") : "MHForever - " + title;
}

} // namespace

int main(int argc, char **argv) {
    mhf::psp::RuntimeOptions options;
    std::string exe;
    std::filesystem::path dump_folder;
    unsigned dump_every = 30;
    double dump_from = 0.0;
    double seconds = 0;
    bool window = false, auto_dialogs = false, fullscreen = false;
    int render_scale = 4, target_fps = 30;
    std::vector<ScriptedPress> script;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--iso" && i + 1 < argc) options.disc_image = argv[++i];
        else if (a == "--exe" && i + 1 < argc) exe = argv[++i];
        else if (a == "--memstick" && i + 1 < argc) options.memstick_root = argv[++i];
        else if (a == "--trace") options.trace_imports = true;
        else if (a == "--quiet-overlays") g_log_overlays = false;
        else if (a == "--dump-frames" && i + 1 < argc) dump_folder = argv[++i];
        else if (a == "--dump-every" && i + 1 < argc) dump_every = std::max(1u, static_cast<unsigned>(std::stoul(argv[++i])));
        else if (a == "--dump-from" && i + 1 < argc) dump_from = std::stod(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::stod(argv[++i]);
        else if (a == "--window") window = true;
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--scale" && i + 1 < argc) render_scale = std::clamp(std::stoi(argv[++i]), 1, 10);
        else if (a == "--fps" && i + 1 < argc) target_fps = std::clamp(std::stoi(argv[++i]), 30, 180);
        else if (a == "--input" && i + 1 < argc) script = parse_input_script(argv[++i]);
        else if (a == "--name" && i + 1 < argc) options.osk_default_text = argv[++i];
        else if (a == "--auto-dialogs") auto_dialogs = true;
        else {
            std::fprintf(stderr, "usage: %s --iso <image.iso> --exe <prepared executable> [--memstick <dir>] [--window]\n", argv[0]);
            return 2;
        }
    }
    if (options.disc_image.empty() || exe.empty()) {
        std::fprintf(stderr, "error: --iso and --exe are required\n");
        return 2;
    }
    options.interactive_dialogs = window && !auto_dialogs; // in a window the player answers the system dialogs
    try {
        Runtime runtime{options};
        runtime.load_elf(exe);
        const auto programs = mhf::psp::generated::mhf1_programs();
        for (std::size_t i = 1; i < programs.size(); ++i) runtime.add_overlay(*programs[i]);
        runtime.register_hook("mh_overlay_prepare", &on_overlay_prepare);
        runtime.register_hook("mh_draw_wait", &on_draw_wait);

        // Unattended runs: a time limit, scripted input and dumps of the screen.
        std::function<void()> extra;
        if (!dump_folder.empty() || seconds > 0 || !script.empty()) {
            if (!dump_folder.empty()) std::filesystem::create_directories(dump_folder);
            std::uint64_t last_slot = 0;
            unsigned frame = 0;
            extra = [=, rt = &runtime]() mutable {
                const std::uint64_t now = rt->now_us();
                if (seconds > 0 && static_cast<double>(now) > seconds * 1e6) {
                    std::fprintf(stderr, "[mhf1] time limit reached\n");
                    rt->dump_threads();
                    rt->exit_program_from_host(0);
                }
                if (!script.empty()) {
                    mhf::psp::ControllerState pad;
                    const double t = static_cast<double>(now) / 1e6;
                    for (const auto &p : script) {
                        if (t < p.at_seconds || t >= p.at_seconds + p.duration) continue;
                        pad.buttons |= p.buttons;
                        if (p.stick_x != 0) pad.lx = static_cast<std::uint8_t>(128 + p.stick_x);
                        if (p.stick_y != 0) pad.ly = static_cast<std::uint8_t>(128 + p.stick_y);
                    }
                    pad.dpad_buttons = pad.buttons & (mhf::psp::buttons::kUp | mhf::psp::buttons::kDown |
                                                      mhf::psp::buttons::kLeft | mhf::psp::buttons::kRight);
                    mhf::psp::set_controller_state(*rt, pad);
                }
                const std::uint64_t slot = now / mhf::psp::kVblankPeriodUs / dump_every;
                if (dump_folder.empty() || slot == last_slot || static_cast<double>(now) < dump_from * 1e6) return;
                last_slot = slot;
                char name[32];
                std::snprintf(name, sizeof name, "frame_%06u.png", ++frame);
                mhf::psp::dump_display(*rt, dump_folder / name);
            };
        }
#if defined(MHF_HAS_FRONTEND)
        mhf::psp::Frontend frontend;
        if (window && frontend.open(window_title(options.disc_image).c_str(), false, 2, true, render_scale, fullscreen, false,
                                    target_fps, mhf::host::KeyBindings{}, mhf::host::PadBindings{})) {
            frontend.attach(runtime, extra);
        } else if (window) {
            std::fprintf(stderr, "error: the game window could not be opened (see the messages above)\n");
            return 3;
        } else if (extra) {
            runtime.kernel_host_poll(extra);
        }
#else
        (void)fullscreen;
        (void)render_scale;
        (void)target_fps;
        if (window) std::fprintf(stderr, "[mhf1] built without the SDL frontend; running headless\n");
        if (extra) runtime.kernel_host_poll(extra);
#endif
        return runtime.run(*programs[0], "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
