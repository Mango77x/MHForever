#pragma once

// PSP runtime for recompiled programs (docs/02-architecture.md, docs/05-runtime-hle.md).
//
// Recompiled guest functions are plain C++ functions `void f(Runtime&, Context&)`. Each PSP thread runs on its own
// host fiber with its own Context; blocking kernel calls switch fibers. Nothing in here is specific to a game.

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mhf::psp {

using Context = psprecomp::AllegrexContext;
class Runtime;
class Kernel;

using GuestFunction = void (*)(Runtime &, Context &);
using HleHandler = void (*)(Runtime &, Context &);
// Host code run on entry to a hooked guest function (the guest function then runs normally).
using HookFunction = void (*)(Runtime &, Context &);

struct HleFunction {
    const char *library;
    std::uint32_t nid;
    const char *name;
    HleHandler handler;
};

struct FunctionEntry {
    std::uint32_t address;
    GuestFunction function;
};

// One imported function of a recompiled program. `hle` is resolved on first use.
struct ImportEntry {
    const char *library;
    std::uint32_t nid;
    std::uint32_t stub;
    const HleFunction *hle;
};

// Everything the generated code of one module provides. Overlays (overlay_id >= 0) share address ranges with other
// overlays; the runtime dispatches into the one that is active in its region (docs/04-recompiler.md §4.4).
struct Program {
    const char *name;
    std::uint32_t entry;
    std::uint32_t gp;
    std::span<const FunctionEntry> functions;
    std::span<ImportEntry> imports;
    std::uint32_t code_start{};
    std::uint32_t code_end{};
    std::int32_t overlay_id{-1};
};

// Error codes returned to the guest (subset of SceKernelErrorCode).
namespace error {
inline constexpr std::uint32_t kUnknownUid = 0x800200CBu;
inline constexpr std::uint32_t kIllegalArgument = 0x800200D2u;
inline constexpr std::uint32_t kNoMemory = 0x80020190u;
inline constexpr std::uint32_t kIllegalPriority = 0x80020193u;
inline constexpr std::uint32_t kIllegalThreadId = 0x80020197u;
inline constexpr std::uint32_t kUnknownThreadId = 0x80020198u;
inline constexpr std::uint32_t kUnknownSemaId = 0x80020199u;
inline constexpr std::uint32_t kUnknownEventFlagId = 0x8002019Au;
inline constexpr std::uint32_t kUnknownMsgPipeId = 0x8002019Bu;
inline constexpr std::uint32_t kNotDormant = 0x800201A4u;
inline constexpr std::uint32_t kDormant = 0x800201A2u;
inline constexpr std::uint32_t kWaitTimeout = 0x800201A8u;
inline constexpr std::uint32_t kWaitCancel = 0x800201A9u;
inline constexpr std::uint32_t kReleaseWait = 0x800201AAu;
inline constexpr std::uint32_t kWaitDelete = 0x800201B5u;
inline constexpr std::uint32_t kSemaZero = 0x800201ADu;
inline constexpr std::uint32_t kSemaOverflow = 0x800201AEu;
inline constexpr std::uint32_t kEventFlagPollFailed = 0x800201AFu;
inline constexpr std::uint32_t kEventFlagNoMultiPermission = 0x800201B0u;
inline constexpr std::uint32_t kEventFlagIllegalWaitPattern = 0x800201B1u;
inline constexpr std::uint32_t kIllegalMode = 0x80020185u;
inline constexpr std::uint32_t kIllegalCount = 0x800201BDu;
inline constexpr std::uint32_t kLwMutexNotFound = 0x800201CAu;
inline constexpr std::uint32_t kLwMutexLocked = 0x800201CBu;
inline constexpr std::uint32_t kLwMutexUnlocked = 0x800201CCu;
inline constexpr std::uint32_t kLwMutexLockOverflow = 0x800201CDu;
inline constexpr std::uint32_t kLwMutexUnlockUnderflow = 0x800201CEu;
inline constexpr std::uint32_t kLwMutexRecursiveNotAllowed = 0x800201CFu;
inline constexpr std::uint32_t kIoFileNotFound = 0x80010002u;
inline constexpr std::uint32_t kIoBadFile = 0x80010009u;
inline constexpr std::uint32_t kIoTooManyOpen = 0x80010018u;
inline constexpr std::uint32_t kIoInvalidArgument = 0x80010016u;
inline constexpr std::uint32_t kIoNoDevice = 0x80020321u;
inline constexpr std::uint32_t kNotImplemented = 0x80020001u;
} // namespace error

struct RuntimeOptions {
    std::uint32_t ram_size = 32u * 1024u * 1024u;
    std::size_t fiber_stack_size = 2u * 1024u * 1024u; // native stack per PSP thread
    bool trace_imports = false;                        // log every HLE call
    // Host folders behind the PSP devices ("ms0:", "disc0:/umd0:", "host0:").
    std::filesystem::path memstick_root = "memstick";
    std::filesystem::path disc_root = "disc";
    // When set, "disc0:"/"umd0:" are served read-only from this ISO image instead of disc_root.
    std::filesystem::path disc_image;
    std::filesystem::path host_root = ".";
};

class Runtime {
public:
    explicit Runtime(RuntimeOptions options = {});
    ~Runtime();
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;

    // The runtime currently executing guest code (one per process).
    [[nodiscard]] static Runtime *active() noexcept;

    [[nodiscard]] psprecomp::GuestMemory &memory() noexcept { return memory_; }
    [[nodiscard]] const RuntimeOptions &options() const noexcept { return options_; }
    [[nodiscard]] Kernel &kernel() noexcept { return *kernel_; }

    // Registration.
    void add_program(const Program &program);
    // Overlays: registered up front, made active when the game loads them.
    void add_overlay(const Program &overlay);
    bool activate_overlay(std::int32_t overlay_id);
    [[nodiscard]] const Program *active_overlay_at(std::uint32_t address) const noexcept;

    // Hooks called by generated code on entry to selected guest functions.
    void register_hook(std::string name, HookFunction function);
    [[nodiscard]] HookFunction find_hook(std::string_view name) const;
    void add_hle(std::span<const HleFunction> functions);
    [[nodiscard]] const HleFunction *find_hle(std::string_view library, std::uint32_t nid) const;

    // Loads a static PSP ELF (the module whose code was recompiled) into guest memory and records its layout.
    void load_elf(const std::filesystem::path &path);

    // Runs the module entry in a new thread and schedules until the program exits. Returns the exit code.
    int run(const Program &program, std::string_view boot_path);

    // Overlay activations so far (diagnostics).
    [[nodiscard]] std::uint64_t overlay_activations() const noexcept { return overlay_activations_; }

    // Generated code interface.
    void call(Context &ctx, std::uint32_t address);  // indirect call; returns when the callee returns
    void jump(Context &ctx, std::uint32_t address) { call(ctx, address); } // indirect tail transfer
    [[nodiscard]] GuestFunction lookup(std::uint32_t address) const noexcept;
    [[noreturn]] void unsupported(std::uint32_t pc, std::uint32_t word, const char *reason);
    [[noreturn]] void arithmetic_overflow(std::uint32_t pc, std::uint32_t word);

    // Fatal error inside a guest call: report, then stop the whole program.
    [[noreturn]] void fatal(const std::string &message);

    // Ends the program (sceKernelExitGame or fatal). Never returns to the caller's thread.
    [[noreturn]] void exit_program(int code);

    void log_unimplemented(const ImportEntry &import);

    // Host integration without exposing the kernel: a hook polled by the scheduler, the time base, and a way for
    // the host to end the program from that hook.
    void kernel_host_poll(std::function<void()> poll);
    [[nodiscard]] std::uint64_t now_us() const;
    void exit_program_from_host(int code);

private:
    RuntimeOptions options_;
    psprecomp::GuestMemory memory_;
    std::unique_ptr<Kernel> kernel_;
    std::vector<FunctionEntry> functions_; // sorted by address (main modules)
    struct OverlayRegion {
        std::uint32_t start{};
        std::uint32_t end{};
        const Program *active{};
    };
    std::vector<OverlayRegion> overlay_regions_;
    std::unordered_map<std::int32_t, const Program *> overlays_;
    std::unordered_map<std::string, HookFunction> hooks_;
    std::unordered_map<std::uint64_t, const HleFunction *> hle_;
    std::unordered_map<std::uint64_t, bool> reported_unimplemented_;
    std::uint64_t overlay_activations_{};
};

// Called by generated import wrappers.
void call_import(Runtime &rt, Context &ctx, ImportEntry &import);

} // namespace mhf::psp
