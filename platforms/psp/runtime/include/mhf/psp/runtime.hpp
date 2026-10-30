#pragma once

// PSP runtime for recompiled programs (docs/02-architecture.md, docs/05-runtime-hle.md).
//
// Recompiled guest functions are plain C++ functions `void f(Runtime&, Context&)`. Each PSP thread runs on its own
// host fiber with its own Context; blocking kernel calls switch fibers. Nothing in here is specific to a game.

#include "mhf/psp/abi.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <thread>

namespace mhf::psp {

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
    // "host0:" is a debugging device: empty (the default) keeps it closed, so a game never reaches the host's files
    // outside the memory stick and the disc (docs/08-multiplayer.md section 4.6). Only developer tools set it.
    std::filesystem::path host_root;
    // Online play (docs/08-multiplayer.md section 1.2): the PSP ad hoc server the game's ad hoc library talks to, as "host" or
    // "host:port" (port 27312). Empty keeps the game offline: nothing is contacted.
    std::string adhoc_server;
    std::string adhoc_nickname = "Hunter";   // the name other players see
    std::string adhoc_product_code;          // ULES01213: servers group players by game
    std::array<std::uint8_t, 6> adhoc_mac{}; // this console's virtual MAC; all zeros: made from the nickname
    // Private room (docs/14-rooms-and-friends.md section 3): with a room code the game's halls are renamed, so only
    // players with the same code meet. Empty: the public halls.
    std::string adhoc_room;
    // The first six bytes of the game's hall names (game data), so that a scan in a private room shows which halls are
    // taken. Prefixes the game joins with are learnt as well.
    std::vector<std::string> adhoc_hall_prefixes;
    // Folder with the free fonts served by sceLibFont (third_party/fonts, copied next to the executable).
    std::filesystem::path font_directory = "fonts";
    // Text the on-screen keyboard starts with when the game's field is empty (e.g. the hunter's name, from the
    // launcher's pseudonym). Without interactive dialogs it is returned as typed.
    std::string osk_default_text = "Hunter";
    // System dialogs (messages, keyboard, save/load) wait for the player and are drawn by the frontend. Off for
    // headless runs and tests, where they answer at once (yes / the default text / the game's own slot).
    bool interactive_dialogs = false;
    // Player volume preferences, 0..100 (100 = as the game mixes it): music is decoded ATRAC audio, effects the voice
    // mixer (sceSasCore).
    int music_volume = 100;
    int sfx_volume = 100;
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
    // Makes the PSP's clock lose `us` microseconds of real time, so that a pause of the host (a menu over the paused
    // game) is not seen by the program as a long stall.
    void skip_time(std::uint64_t us);
    // The player's volume preferences (0..100), changeable while the program runs.
    void set_volumes(int music, int sfx) noexcept {
        options_.music_volume = music;
        options_.sfx_volume = sfx;
    }
    void exit_program_from_host(int code);
    // Online play: the private room the game's next scan and next join use, "" for the public halls
    // (docs/14-rooms-and-friends.md section 3.4). A code that is not valid means the public halls. Changing it never
    // takes the player out of a hall: it applies when the game next enters one.
    void set_adhoc_room(std::string_view code);
    [[nodiscard]] std::string adhoc_room();
    // The game is in a hall of the ad hoc network now.
    [[nodiscard]] bool adhoc_in_hall();
    // The hall the game is in: whether it is in one, the room it belongs to ("" the public halls) and the names of the
    // other players there, as the server reports them.
    struct AdhocHall {
        bool in = false;
        std::string room;
        std::vector<std::string> players;
    };
    [[nodiscard]] AdhocHall adhoc_hall();
    // Online play: another ad hoc server ("host" or "host:port"). It takes over as soon as that cuts nothing off: at
    // once in the village, after the game left the hall or the quest it is in otherwise. adhoc_server() is the one
    // chosen; adhoc_server_pending() says it is still waiting.
    void set_adhoc_server(std::string_view server);
    [[nodiscard]] std::string adhoc_server();
    [[nodiscard]] bool adhoc_server_pending();
    // Prints the PSP threads and what they wait for to stderr (debugging hangs).
    void dump_threads();
    // Progress marks for the stall watchdog (MHF_WATCHDOG=<seconds>, runtime.cpp): every frame the game presents, and the last
    // system call it made. Cheap enough to keep always on.
    void note_frame() noexcept { frames_.fetch_add(1, std::memory_order_relaxed); }
    void note_import(const char *name) noexcept {
        last_import_.store(name, std::memory_order_relaxed);
        imports_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    RuntimeOptions options_;
    psprecomp::GuestMemory memory_;
    std::unique_ptr<Kernel> kernel_;
    std::thread ticker_;                 // raises abi::interrupt_pending every few milliseconds
    std::atomic<bool> ticker_stop_{false};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> imports_{0};
    std::atomic<const char *> last_import_{nullptr};
    std::vector<FunctionEntry> functions_; // sorted by address (main modules)
    std::vector<std::uint32_t> import_stubs_; // sorted; always native even when interpreting everything
    bool interpret_only_{};                   // MHF_INTERPRET_ONLY=1: run guest code with the interpreter (tests)
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

// Room codes (docs/14-rooms-and-friends.md section 3): "k7qf-29xm" -> "K7QF29XM", "" when not a valid code; a new
// random one; and the way players read and type one, "K7QF-29XM".
[[nodiscard]] std::string adhoc_room_code(std::string_view text);
[[nodiscard]] std::string new_adhoc_room_code();
[[nodiscard]] std::string format_adhoc_room_code(const std::string &code);

// Resolves and runs an imported function (also used by generated import wrappers through abi::call_import).
void call_import(Runtime &rt, Context &ctx, ImportEntry &import);

} // namespace mhf::psp
