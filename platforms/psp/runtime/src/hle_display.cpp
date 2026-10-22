// sceDisplay and sceCtrl. The host reads DisplayState and feeds ControllerState (mhf/psp/host.hpp).

#include "hle.hpp"

#include <cstdio>
#include "kernel.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "mhf/psp/host.hpp"

#include <unordered_map>

namespace mhf::psp {

namespace {
struct VideoState {
    DisplayState display;
    ControllerState controller;
    std::uint32_t sampling_cycle{};
    std::uint32_t sampling_mode{};
    int display_mode{};
    int width{480};
    int height{272};
};
std::unordered_map<const Runtime *, VideoState> g_video;
VideoState &video(Runtime &rt) { return g_video[&rt]; }

std::uint64_t vcount(Runtime &rt) { return rt.kernel().now_us() / kVblankPeriodUs; }
std::uint64_t next_vblank(Runtime &rt) { return (vcount(rt) + 1) * kVblankPeriodUs; }
} // namespace

namespace hle {
void display_release(Runtime &rt) { g_video.erase(&rt); }
} // namespace hle

const DisplayState &display_state(Runtime &rt) { return video(rt).display; }
void set_controller_state(Runtime &rt, const ControllerState &state) { video(rt).controller = state; }
const ControllerState &controller_state(Runtime &rt) { return video(rt).controller; }

namespace hle {
namespace {

void DisplaySetMode(Runtime &rt, Context &ctx) {
    auto &v = video(rt);
    v.display_mode = sarg(ctx, 0);
    v.width = sarg(ctx, 1);
    v.height = sarg(ctx, 2);
    ret(ctx, 0);
}

void DisplayGetMode(Runtime &rt, Context &ctx) {
    const auto &v = video(rt);
    if (arg(ctx, 0) != 0) rt.memory().store32(arg(ctx, 0), static_cast<std::uint32_t>(v.display_mode));
    if (arg(ctx, 1) != 0) rt.memory().store32(arg(ctx, 1), static_cast<std::uint32_t>(v.width));
    if (arg(ctx, 2) != 0) rt.memory().store32(arg(ctx, 2), static_cast<std::uint32_t>(v.height));
    ret(ctx, 0);
}

void DisplaySetFrameBuf(Runtime &rt, Context &ctx) {
    auto &d = video(rt).display;
    auto &p = perf();
    ++p.frames;
    const std::uint64_t now = rt.now_us();
    if (p.last_report == 0) p.last_report = now;
    if (now - p.last_report >= 5'000'000u) {
        const double seconds = static_cast<double>(now - p.last_report) / 1e6;
        // CPU time of the game's thread (every guest thread is a fiber on it) and of the drawing thread, as a share of one
        // core: whichever is near 100 % is what limits the speed.
        static std::uint64_t last_game_cpu = 0, last_ge_cpu = 0;
        const std::uint64_t game_cpu = thread_cpu_us(), ge_cpu = ge_thread_cpu_us().load(std::memory_order_relaxed);
        std::fprintf(stderr, "[perf] cpu: game thread %.0f%%, drawing thread %.0f%% of one core\n",
                     100.0 * static_cast<double>(game_cpu - last_game_cpu) / (seconds * 1e6),
                     100.0 * static_cast<double>(ge_cpu - last_ge_cpu) / (seconds * 1e6));
        last_game_cpu = game_cpu;
        last_ge_cpu = ge_cpu;
        static std::uint64_t last_transform = 0, last_assemble = 0;
        const std::uint64_t transform = ge_detail().transform_us.load(std::memory_order_relaxed);
        const std::uint64_t assemble = ge_detail().assemble_us.load(std::memory_order_relaxed);
        const double per_frame = p.frames ? 1000.0 * static_cast<double>(p.frames) : 1.0;
        std::fprintf(stderr, "[perf] display lists per frame: vertices %.1f ms, primitives and drawing %.1f ms\n",
                     static_cast<double>(transform - last_transform) / per_frame * 1000.0 / 1000.0,
                     static_cast<double>(assemble - last_assemble) / per_frame * 1000.0 / 1000.0);
        last_transform = transform;
        last_assemble = assemble;
        std::fprintf(stderr, "[perf] %.1f fps, display lists %.0f%% of the time (%.1f ms per frame)\n",
                     static_cast<double>(p.frames) / seconds, 100.0 * static_cast<double>(p.ge_us) / (seconds * 1e6),
                     p.frames ? static_cast<double>(p.ge_us) / 1000.0 / static_cast<double>(p.frames) : 0.0);
        p = PerfCounters{0, 0, now};
    }
    d.framebuffer = arg(ctx, 0);
    if (arg(ctx, 1) != 0) d.stride = arg(ctx, 1);
    d.pixel_format = arg(ctx, 2);
    ++d.frames;
    ge_present(rt, d.framebuffer, d.stride, d.pixel_format);
    ret(ctx, 0);
}

void DisplayGetFrameBuf(Runtime &rt, Context &ctx) {
    const auto &d = video(rt).display;
    if (arg(ctx, 0) != 0) rt.memory().store32(arg(ctx, 0), d.framebuffer);
    if (arg(ctx, 1) != 0) rt.memory().store32(arg(ctx, 1), d.stride);
    if (arg(ctx, 2) != 0) rt.memory().store32(arg(ctx, 2), d.pixel_format);
    ret(ctx, 0);
}

void DisplayWaitVblankStart(Runtime &rt, Context &ctx) {
    rt.kernel().wait(Kernel::WaitType::Vblank, 0, next_vblank(rt));
    ret(ctx, 0);
}

void DisplayGetVcount(Runtime &rt, Context &ctx) { ret(ctx, static_cast<std::uint32_t>(vcount(rt))); }

void DisplayIsVblank(Runtime &rt, Context &ctx) {
    // The visible part lasts 272 of 286 lines.
    const std::uint64_t phase = rt.kernel().now_us() % kVblankPeriodUs;
    ret(ctx, phase >= kVblankPeriodUs * 272 / 286 ? 1u : 0u);
}

void DisplayGetCurrentHcount(Runtime &rt, Context &ctx) {
    ret(ctx, static_cast<std::uint32_t>((rt.kernel().now_us() % kVblankPeriodUs) * 286 / kVblankPeriodUs));
}

void DisplayGetAccumulatedHcount(Runtime &rt, Context &ctx) {
    ret(ctx, static_cast<std::uint32_t>(rt.kernel().now_us() * 286 / kVblankPeriodUs));
}

void DisplayGetFramePerSec(Runtime &, Context &ctx) {
    ret(ctx, std::bit_cast<std::uint32_t>(59.9400599f));
    ctx.fpr[0] = 59.9400599f;
}

void DisplayZero(Runtime &, Context &ctx) { ret(ctx, 0); }
void DisplayIsForeground(Runtime &, Context &ctx) { ret(ctx, 1); }

// SceCtrlData: u32 timestamp, u32 buttons, u8 lx, u8 ly, u8 reserved[6].
void write_samples(Runtime &rt, std::uint32_t address, int count, bool negative) {
    const auto &c = video(rt).controller;
    const auto stamp = static_cast<std::uint32_t>(rt.kernel().now_us());
    auto &mem = rt.memory();
    for (int i = 0; i < count; ++i) {
        const std::uint32_t at = address + static_cast<std::uint32_t>(i) * 16u;
        mem.store32(at, stamp);
        mem.store32(at + 4, negative ? ~c.buttons : c.buttons);
        mem.store8(at + 8, c.lx);
        mem.store8(at + 9, c.ly);
        for (std::uint32_t r = 10; r < 16; ++r) mem.store8(at + r, 0);
    }
}

void CtrlSetSamplingCycle(Runtime &rt, Context &ctx) {
    ret(ctx, video(rt).sampling_cycle);
    video(rt).sampling_cycle = arg(ctx, 0);
}

void CtrlGetSamplingCycle(Runtime &rt, Context &ctx) {
    rt.memory().store32(arg(ctx, 0), video(rt).sampling_cycle);
    ret(ctx, 0);
}

void CtrlSetSamplingMode(Runtime &rt, Context &ctx) {
    ret(ctx, video(rt).sampling_mode);
    video(rt).sampling_mode = arg(ctx, 0);
}

void CtrlGetSamplingMode(Runtime &rt, Context &ctx) {
    rt.memory().store32(arg(ctx, 0), video(rt).sampling_mode);
    ret(ctx, 0);
}

int sample_count(Context &ctx) {
    const int n = sarg(ctx, 1);
    return n < 1 ? 1 : (n > 64 ? 64 : n);
}

void CtrlPeekBufferPositive(Runtime &rt, Context &ctx) {
    write_samples(rt, arg(ctx, 0), sample_count(ctx), false);
    ret(ctx, static_cast<std::uint32_t>(sample_count(ctx)));
}

void CtrlPeekBufferNegative(Runtime &rt, Context &ctx) {
    write_samples(rt, arg(ctx, 0), sample_count(ctx), true);
    ret(ctx, static_cast<std::uint32_t>(sample_count(ctx)));
}

// Read variants block until the next sample, which the PSP takes once per frame by default.
void CtrlReadBufferPositive(Runtime &rt, Context &ctx) {
    const std::uint32_t address = arg(ctx, 0);
    const int count = sample_count(ctx);
    rt.kernel().wait(Kernel::WaitType::Vblank, 0, next_vblank(rt));
    write_samples(rt, address, count, false);
    ret(ctx, static_cast<std::uint32_t>(count));
}

void CtrlReadBufferNegative(Runtime &rt, Context &ctx) {
    const std::uint32_t address = arg(ctx, 0);
    const int count = sample_count(ctx);
    rt.kernel().wait(Kernel::WaitType::Vblank, 0, next_vblank(rt));
    write_samples(rt, address, count, true);
    ret(ctx, static_cast<std::uint32_t>(count));
}

// Latch: {u32 make, u32 break, u32 press, u32 release}. We report the current press state only.
std::uint32_t g_previous_buttons = 0;
void CtrlLatch(Runtime &rt, Context &ctx) {
    const std::uint32_t now = video(rt).controller.buttons;
    const std::uint32_t address = arg(ctx, 0);
    rt.memory().store32(address + 0, now & ~g_previous_buttons);
    rt.memory().store32(address + 4, g_previous_buttons & ~now);
    rt.memory().store32(address + 8, now);
    rt.memory().store32(address + 12, ~now);
    g_previous_buttons = now;
    ret(ctx, 1);
}

void CtrlZero(Runtime &, Context &ctx) { ret(ctx, 0); }

const HleFunction kFunctions[] = {
    {"sceDisplay", 0x0E20F177u, "sceDisplaySetMode", &DisplaySetMode},
    {"sceDisplay", 0xDEA197D4u, "sceDisplayGetMode", &DisplayGetMode},
    {"sceDisplay", 0x289D82FEu, "sceDisplaySetFrameBuf", &DisplaySetFrameBuf},
    {"sceDisplay", 0xEEDA2E54u, "sceDisplayGetFrameBuf", &DisplayGetFrameBuf},
    {"sceDisplay", 0x984C27E7u, "sceDisplayWaitVblankStart", &DisplayWaitVblankStart},
    {"sceDisplay", 0x46F186C3u, "sceDisplayWaitVblankStartCB", &DisplayWaitVblankStart},
    {"sceDisplay", 0x36CDFADEu, "sceDisplayWaitVblank", &DisplayWaitVblankStart},
    {"sceDisplay", 0x8EB9EC49u, "sceDisplayWaitVblankCB", &DisplayWaitVblankStart},
    {"sceDisplay", 0x9C6EAAD7u, "sceDisplayGetVcount", &DisplayGetVcount},
    {"sceDisplay", 0x4D4E10ECu, "sceDisplayIsVblank", &DisplayIsVblank},
    {"sceDisplay", 0x773DD3A3u, "sceDisplayGetCurrentHcount", &DisplayGetCurrentHcount},
    {"sceDisplay", 0x210EAB3Au, "sceDisplayGetAccumulatedHcount", &DisplayGetAccumulatedHcount},
    {"sceDisplay", 0xDBA6C4C4u, "sceDisplayGetFramePerSec", &DisplayGetFramePerSec},
    {"sceDisplay", 0x7ED59BC4u, "sceDisplaySetHoldMode", &DisplayZero},
    {"sceDisplay", 0xA544C486u, "sceDisplaySetResumeMode", &DisplayZero},
    {"sceDisplay", 0xB4F378FAu, "sceDisplayIsForeground", &DisplayIsForeground},
    {"sceCtrl", 0x6A2774F3u, "sceCtrlSetSamplingCycle", &CtrlSetSamplingCycle},
    {"sceCtrl", 0x02BAAD91u, "sceCtrlGetSamplingCycle", &CtrlGetSamplingCycle},
    {"sceCtrl", 0x1F4011E6u, "sceCtrlSetSamplingMode", &CtrlSetSamplingMode},
    {"sceCtrl", 0xDA6B76A1u, "sceCtrlGetSamplingMode", &CtrlGetSamplingMode},
    {"sceCtrl", 0x3A622550u, "sceCtrlPeekBufferPositive", &CtrlPeekBufferPositive},
    {"sceCtrl", 0xC152080Au, "sceCtrlPeekBufferNegative", &CtrlPeekBufferNegative},
    {"sceCtrl", 0x1F803938u, "sceCtrlReadBufferPositive", &CtrlReadBufferPositive},
    {"sceCtrl", 0x60B81F86u, "sceCtrlReadBufferNegative", &CtrlReadBufferNegative},
    {"sceCtrl", 0x0B588501u, "sceCtrlReadLatch", &CtrlLatch},
    {"sceCtrl", 0xB1D0E5CDu, "sceCtrlPeekLatch", &CtrlLatch},
    {"sceCtrl", 0xA7144800u, "sceCtrlSetIdleCancelThreshold", &CtrlZero},
    {"sceCtrl", 0x687660FAu, "sceCtrlGetIdleCancelThreshold", &CtrlZero},
};

} // namespace

std::span<const HleFunction> display_functions() { return kFunctions; }

PerfCounters &perf() {
    static PerfCounters counters;
    return counters;
}

std::uint64_t thread_cpu_us() {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    const auto to_100ns = [](const FILETIME &f) {
        return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32u) | f.dwLowDateTime;
    };
    return (to_100ns(kernel) + to_100ns(user)) / 10u;
#else
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000u;
#endif
}

GeDetail &ge_detail() {
    static GeDetail detail;
    return detail;
}

std::atomic<std::uint64_t> &ge_thread_cpu_us() {
    static std::atomic<std::uint64_t> value{0};
    return value;
}

} // namespace hle
} // namespace mhf::psp
