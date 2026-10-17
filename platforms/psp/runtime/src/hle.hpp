#pragma once

// Helpers shared by the HLE modules (one .cpp per PSP library group).

#include "mhf/psp/runtime.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace mhf::psp::hle {

// Arguments 0..7 live in $a0-$a3, $t0-$t3; further ones on the caller's stack.
[[nodiscard]] inline std::uint32_t arg(const Context &ctx, unsigned index) { return ctx.gpr[4u + index]; }
[[nodiscard]] inline std::int32_t sarg(const Context &ctx, unsigned index) {
    return static_cast<std::int32_t>(ctx.gpr[4u + index]);
}
inline void ret(Context &ctx, std::uint32_t value) { ctx.gpr[2] = value; }
inline void ret64(Context &ctx, std::uint64_t value) {
    ctx.gpr[2] = static_cast<std::uint32_t>(value);
    ctx.gpr[3] = static_cast<std::uint32_t>(value >> 32u);
}

[[nodiscard]] std::string read_string(Runtime &rt, std::uint32_t address, std::size_t max_length = 256);

// Sets the current directory used for relative PSP paths (IoFileMgr).
void set_working_directory(Runtime &rt, const std::string &psp_path);

// Runs one sceUtility savedata request (hle_savedata.cpp) and stores its result in the parameter block.
std::uint32_t execute_savedata(Runtime &rt, std::uint32_t params);

// Frame-time accounting, reported by sceDisplaySetFrameBuf every few seconds ("[perf]" lines).
struct PerfCounters {
    std::uint64_t ge_us{};        // host time spent running display lists since the last report
    std::uint64_t frames{};       // frames presented since the last report
    std::uint64_t last_report{};  // host time of the last report
};
PerfCounters &perf();

// Queues a flip to the GPU backend, in order with the display lists (no-op without a backend).
// Stops the GE worker thread (at runtime shutdown).
void ge_shutdown(Runtime &rt);
void ge_present(Runtime &rt, std::uint32_t address, std::uint32_t stride, std::uint32_t format);

// Registration of each module group (called from Runtime's constructor).
std::span<const HleFunction> thread_functions();   // ThreadManForUser, Kernel_Library
std::span<const HleFunction> sysmem_functions();   // SysMemUserForUser
std::span<const HleFunction> io_functions();       // IoFileMgrForUser, StdioForUser
std::span<const HleFunction> system_functions();   // LoadExecForUser, ModuleMgrForUser, UtilsForUser, sceRtc, ...
std::span<const HleFunction> display_functions();  // sceDisplay, sceCtrl
std::span<const HleFunction> misc_functions();     // interrupts, vtimers, modules, UMD, small services
std::span<const HleFunction> ge_functions();       // sceGe_user
std::span<const HleFunction> media_functions();    // sceAudio, sceSasCore, sceAtrac3plus, sceMpeg, dialogs
std::span<const HleFunction> font_functions();     // sceLibFont (free fonts)

} // namespace mhf::psp::hle
