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

// Registration of each module group (called from Runtime's constructor).
std::span<const HleFunction> thread_functions();   // ThreadManForUser, Kernel_Library
std::span<const HleFunction> sysmem_functions();   // SysMemUserForUser
std::span<const HleFunction> io_functions();       // IoFileMgrForUser, StdioForUser
std::span<const HleFunction> system_functions();   // LoadExecForUser, ModuleMgrForUser, UtilsForUser, sceRtc, ...
std::span<const HleFunction> display_functions();  // sceDisplay, sceCtrl

} // namespace mhf::psp::hle
