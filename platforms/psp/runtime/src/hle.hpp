#pragma once

// Helpers shared by the HLE modules (one .cpp per PSP library group).

#include "mhf/psp/runtime.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>
#include <filesystem>
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

// Joins the part of a PSP path after "device:" onto `root`. Nothing is returned when the path would leave the root
// (".." above it) or contains a component the host could read as something else than a plain name: a ':' (a Windows
// drive such as "C:", or an NTFS stream) or a character Windows forbids in file names.
[[nodiscard]] std::optional<std::filesystem::path> resolve_under_root(const std::filesystem::path &root,
                                                                       const std::string &rest);

// Sets the current directory used for relative PSP paths (IoFileMgr).
void set_working_directory(Runtime &rt, const std::string &psp_path);
[[nodiscard]] std::string working_directory(Runtime &rt);

// Per-runtime HLE state lives in module-level tables keyed by the Runtime; the destructor drops each entry (closing
// host files and so on) so that a later Runtime at the same address starts clean.
void io_release(Runtime &rt);
void display_release(Runtime &rt);
void ge_release(Runtime &rt);

// Runs one sceUtility savedata request (hle_savedata.cpp) and stores its result in the parameter block.
// `chosen` is the slot the player picked in the list dialog (list modes); without it the request's own selection
// is used.
std::uint32_t execute_savedata(Runtime &rt, std::uint32_t params, const std::optional<std::string> &chosen = {});

// What the savedata dialog should show before running a request (hle_utility.cpp draws it).
struct SavedataSlot {
    std::string name;                // save name (folder suffix)
    std::string title, detail, date; // from the slot's PARAM.SFO, empty when the slot is free
    bool exists = false;
};
struct SavedataPlan {
    enum class Ui { None, Toast, Confirm, List } ui = Ui::None;
    enum class Action { Load, Save, Delete } action = Action::Load;
    std::uint32_t mode = 0;
    std::vector<SavedataSlot> slots; // Confirm: the one slot; List: the choices
    int focus = 0;
    std::string new_title, new_detail; // what a save would write
};
[[nodiscard]] SavedataPlan savedata_plan(Runtime &rt, std::uint32_t params);

// Frame-time accounting, reported by sceDisplaySetFrameBuf every few seconds ("[perf]" lines).
struct PerfCounters {
    std::uint64_t ge_us{};        // host time spent running display lists since the last report
    std::uint64_t frames{};       // frames presented since the last report
    std::uint64_t last_report{};  // host time of the last report
};
PerfCounters &perf();

// CPU time used so far by the calling thread, in microseconds (0 where the system cannot tell).
std::uint64_t thread_cpu_us();
// Published by the GE worker thread every time around its loop, read when the "[perf]" line is printed.
std::atomic<std::uint64_t> &ge_thread_cpu_us();

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
std::span<const HleFunction> media_functions();    // sceAudio, sceMpeg, dialogs
std::span<const HleFunction> sas_functions();      // sceSasCore (voice mixer)
std::span<const HleFunction> utility_functions();  // sceUtility dialogs
std::span<const HleFunction> atrac_functions();    // sceAtrac3plus (music decoding)
std::span<const HleFunction> font_functions();     // sceLibFont (free fonts)

} // namespace mhf::psp::hle
