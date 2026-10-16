#pragma once

// The stable interface between recompiled code and the runtime (docs/04-recompiler.md §5).
//
// Generated sources include only this header (through generated.hpp), never runtime.hpp, so that changing the
// runtime does not force recompiling a million lines of generated code. Keep it small and change it rarely: every
// change here rebuilds every recompiled game.

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <span>

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

// Calls made by generated code (implemented in runtime.cpp).
namespace abi {
[[nodiscard]] psprecomp::GuestMemory &memory(Runtime &rt) noexcept;
void call(Runtime &rt, Context &ctx, std::uint32_t address);   // indirect call; returns when the callee returns
void jump(Runtime &rt, Context &ctx, std::uint32_t address);   // indirect tail transfer
[[noreturn]] void unsupported(Runtime &rt, std::uint32_t pc, std::uint32_t word, const char *reason);
[[noreturn]] void arithmetic_overflow(Runtime &rt, std::uint32_t pc, std::uint32_t word);
[[nodiscard]] HookFunction find_hook(Runtime &rt, const char *name);
void call_import(Runtime &rt, Context &ctx, ImportEntry &import);
} // namespace abi

} // namespace mhf::psp
