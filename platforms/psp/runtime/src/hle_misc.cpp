// InterruptManager, virtual timers, dispatch control, modules, UMD, and small system services.

#include "hle.hpp"
#include "kernel.hpp"

#include "psprecomp/common.hpp"

#include <cstdio>
#include <vector>

namespace mhf::psp::hle {

namespace {

Kernel &k(Runtime &rt) { return rt.kernel(); }

// ---------------------------------------------------------------- InterruptManager (vblank only)

void RegisterSubIntrHandler(Runtime &rt, Context &ctx) {
    if (sarg(ctx, 0) != Kernel::kVblankInterrupt) {
        std::fprintf(stderr, "[hle] sub-interrupt handler for interrupt %d ignored\n", sarg(ctx, 0));
        ret(ctx, 0);
        return;
    }
    auto &h = k(rt).vblank_handlers[sarg(ctx, 1)];
    h.handler = arg(ctx, 2);
    h.argument = arg(ctx, 3);
    h.enabled = false;
    ret(ctx, 0);
}

void ReleaseSubIntrHandler(Runtime &rt, Context &ctx) {
    if (sarg(ctx, 0) == Kernel::kVblankInterrupt) k(rt).vblank_handlers.erase(sarg(ctx, 1));
    ret(ctx, 0);
}

void EnableSubIntr(Runtime &rt, Context &ctx) {
    if (sarg(ctx, 0) == Kernel::kVblankInterrupt) {
        auto it = k(rt).vblank_handlers.find(sarg(ctx, 1));
        if (it == k(rt).vblank_handlers.end()) {
            ret(ctx, 0x80020068u); // SCE_KERNEL_ERROR_NOTFOUND_HANDLER
            return;
        }
        it->second.enabled = true;
    }
    ret(ctx, 0);
}

void DisableSubIntr(Runtime &rt, Context &ctx) {
    if (sarg(ctx, 0) == Kernel::kVblankInterrupt) {
        auto it = k(rt).vblank_handlers.find(sarg(ctx, 1));
        if (it != k(rt).vblank_handlers.end()) it->second.enabled = false;
    }
    ret(ctx, 0);
}

// ---------------------------------------------------------------- virtual timers

Kernel::VTimer *vtimer(Runtime &rt, int uid) {
    auto it = k(rt).vtimers.find(uid);
    return it != k(rt).vtimers.end() ? &it->second : nullptr;
}

constexpr std::uint32_t kUnknownVTimer = 0x800201B2u;

void CreateVTimer(Runtime &rt, Context &ctx) {
    Kernel::VTimer t;
    t.uid = k(rt).next_uid();
    t.name = read_string(rt, arg(ctx, 0), 32);
    const int uid = t.uid;
    k(rt).vtimers.emplace(uid, std::move(t));
    ret(ctx, static_cast<std::uint32_t>(uid));
}

void DeleteVTimer(Runtime &rt, Context &ctx) {
    ret(ctx, k(rt).vtimers.erase(sarg(ctx, 0)) != 0 ? 0u : kUnknownVTimer);
}

void StartVTimer(Runtime &rt, Context &ctx) {
    auto *t = vtimer(rt, sarg(ctx, 0));
    if (t == nullptr) {
        ret(ctx, kUnknownVTimer);
        return;
    }
    if (t->running) {
        ret(ctx, 1);
        return;
    }
    t->running = true;
    t->started_at = k(rt).now_us();
    ret(ctx, 0);
}

void StopVTimer(Runtime &rt, Context &ctx) {
    auto *t = vtimer(rt, sarg(ctx, 0));
    if (t == nullptr) {
        ret(ctx, kUnknownVTimer);
        return;
    }
    if (!t->running) {
        ret(ctx, 0);
        return;
    }
    t->accumulated = k(rt).vtimer_time(*t);
    t->running = false;
    ret(ctx, 1);
}

void GetVTimerTimeWide(Runtime &rt, Context &ctx) {
    auto *t = vtimer(rt, sarg(ctx, 0));
    ret64(ctx, t != nullptr ? k(rt).vtimer_time(*t) : 0u);
}

void GetVTimerBaseWide(Runtime &rt, Context &ctx) {
    auto *t = vtimer(rt, sarg(ctx, 0));
    ret64(ctx, t != nullptr ? t->started_at : 0u);
}

// (uid, u64 time in a2:a3, handler t0, common t1)
void SetVTimerHandlerWide(Runtime &rt, Context &ctx) {
    auto *t = vtimer(rt, sarg(ctx, 0));
    if (t == nullptr) {
        ret(ctx, kUnknownVTimer);
        return;
    }
    t->schedule = arg(ctx, 2) | (static_cast<std::uint64_t>(arg(ctx, 3)) << 32u);
    t->handler = arg(ctx, 4);
    t->common = arg(ctx, 5);
    ret(ctx, 0);
}

// (uid, SceKernelSysClock *time, handler, common)
void SetVTimerHandler(Runtime &rt, Context &ctx) {
    auto *t = vtimer(rt, sarg(ctx, 0));
    if (t == nullptr) {
        ret(ctx, kUnknownVTimer);
        return;
    }
    const std::uint32_t clock = arg(ctx, 1);
    t->schedule = clock == 0 ? 0u
                             : (rt.memory().load32(clock) |
                                (static_cast<std::uint64_t>(rt.memory().load32(clock + 4)) << 32u));
    t->handler = arg(ctx, 2);
    t->common = arg(ctx, 3);
    ret(ctx, 0);
}

void CancelVTimerHandler(Runtime &rt, Context &ctx) {
    auto *t = vtimer(rt, sarg(ctx, 0));
    if (t == nullptr) {
        ret(ctx, kUnknownVTimer);
        return;
    }
    t->handler = 0;
    ret(ctx, 0);
}

// Cooperative scheduling: nothing preempts a running thread anyway.
int g_dispatch_suspended = 0;
void SuspendDispatchThread(Runtime &, Context &ctx) {
    ret(ctx, g_dispatch_suspended == 0 ? 1u : 0u);
    ++g_dispatch_suspended;
}
void ResumeDispatchThread(Runtime &, Context &ctx) {
    if (g_dispatch_suspended > 0) --g_dispatch_suspended;
    ret(ctx, 0);
}

// ---------------------------------------------------------------- modules
// Firmware and game modules are high-level emulated: their exports are HLE functions, so loading only hands out ids.

int g_next_module = 0x2000;

void LoadModule(Runtime &rt, Context &ctx) {
    std::fprintf(stderr, "[hle] sceKernelLoadModule(\"%s\") -> HLE module\n", read_string(rt, arg(ctx, 0)).c_str());
    ret(ctx, static_cast<std::uint32_t>(g_next_module++));
}

void LoadModuleByID(Runtime &, Context &ctx) {
    std::fprintf(stderr, "[hle] sceKernelLoadModuleByID(fd %d) -> HLE module\n", sarg(ctx, 0));
    ret(ctx, static_cast<std::uint32_t>(g_next_module++));
}

void StartModule(Runtime &rt, Context &ctx) {
    if (arg(ctx, 3) != 0) rt.memory().store32(arg(ctx, 3), 0);
    ret(ctx, arg(ctx, 0));
}

void StopModule(Runtime &rt, Context &ctx) {
    if (arg(ctx, 3) != 0) rt.memory().store32(arg(ctx, 3), 0);
    ret(ctx, 0);
}

void UnloadModule(Runtime &, Context &ctx) { ret(ctx, 0); }
void GetModuleId(Runtime &, Context &ctx) { ret(ctx, 0x1001u); }
void StopUnloadSelfModuleWithStatus(Runtime &rt, Context &ctx) { rt.exit_program(sarg(ctx, 0)); }

void UtilityLoadModule(Runtime &, Context &ctx) { ret(ctx, 0); }
void UtilityUnloadModule(Runtime &, Context &ctx) { ret(ctx, 0); }

void SetCompilerVersion(Runtime &, Context &ctx) { ret(ctx, 0); }

// ---------------------------------------------------------------- volatile memory (the 4 MB the system can lend)

bool g_volatile_locked = false;
bool g_volatile_reserved = false;

void VolatileMemLock(Runtime &rt, Context &ctx) {
    if (!g_volatile_reserved) {
        g_volatile_reserved = k(rt).sysmem().reserve(1, "volatile", 0x08400000u, 0x00400000u).has_value();
    }
    if (g_volatile_locked) {
        ret(ctx, 0x802B0200u); // SCE_KERNEL_ERROR_POWER_VMEM_IN_USE
        return;
    }
    g_volatile_locked = true;
    if (arg(ctx, 1) != 0) rt.memory().store32(arg(ctx, 1), 0x08400000u);
    if (arg(ctx, 2) != 0) rt.memory().store32(arg(ctx, 2), 0x00400000u);
    ret(ctx, 0);
}

void VolatileMemUnlock(Runtime &, Context &ctx) {
    g_volatile_locked = false;
    ret(ctx, 0);
}

// ---------------------------------------------------------------- UMD

void UmdCheckMedium(Runtime &, Context &ctx) { ret(ctx, 1); }
void UmdActivate(Runtime &, Context &ctx) { ret(ctx, 0); }
void UmdGetDriveStat(Runtime &, Context &ctx) { ret(ctx, 0x32u); } // present | initialized | ready
void UmdWaitDriveStat(Runtime &, Context &ctx) { ret(ctx, 0); }
void UmdGetErrorStat(Runtime &, Context &ctx) { ret(ctx, 0); }
void UmdRegisterCallback(Runtime &, Context &ctx) { ret(ctx, 0); }
void UmdGetDiscInfo(Runtime &rt, Context &ctx) {
    if (arg(ctx, 0) != 0) rt.memory().store32(arg(ctx, 0) + 4, 0x10u); // UMD type: game
    ret(ctx, 0);
}

// ---------------------------------------------------------------- misc

void ImposeSetLanguageMode(Runtime &, Context &ctx) { ret(ctx, 0); }
void ImposeGetLanguageMode(Runtime &rt, Context &ctx) {
    if (arg(ctx, 0) != 0) rt.memory().store32(arg(ctx, 0), 1); // English
    if (arg(ctx, 1) != 0) rt.memory().store32(arg(ctx, 1), 1); // cross confirms
    ret(ctx, 0);
}

void DmacMemcpy(Runtime &rt, Context &ctx) {
    const std::uint32_t size = arg(ctx, 2);
    std::vector<std::uint8_t> buffer(size);
    rt.memory().copy_out(arg(ctx, 1), buffer);
    rt.memory().copy_in(arg(ctx, 0), buffer);
    ret(ctx, 0);
}

void GetOpenPSID(Runtime &rt, Context &ctx) {
    // A fixed, made-up console id: the game only uses it as an opaque value.
    static constexpr std::uint8_t id[16] = {0x10, 0x02, 0xA3, 0x44, 0x13, 0xF5, 0x93, 0xB0,
                                            0xCC, 0x6E, 0xD1, 0x32, 0x27, 0x85, 0x0F, 0x9B};
    rt.memory().copy_in(arg(ctx, 0), id);
    ret(ctx, 0);
}

const HleFunction kFunctions[] = {
    {"InterruptManager", 0xCA04A2B9u, "sceKernelRegisterSubIntrHandler", &RegisterSubIntrHandler},
    {"InterruptManager", 0xD61E6961u, "sceKernelReleaseSubIntrHandler", &ReleaseSubIntrHandler},
    {"InterruptManager", 0xFB8E22ECu, "sceKernelEnableSubIntr", &EnableSubIntr},
    {"InterruptManager", 0x8A389411u, "sceKernelDisableSubIntr", &DisableSubIntr},
    {"ThreadManForUser", 0x20FFF560u, "sceKernelCreateVTimer", &CreateVTimer},
    {"ThreadManForUser", 0x328F9E52u, "sceKernelDeleteVTimer", &DeleteVTimer},
    {"ThreadManForUser", 0xC68D9437u, "sceKernelStartVTimer", &StartVTimer},
    {"ThreadManForUser", 0xD0AEEE87u, "sceKernelStopVTimer", &StopVTimer},
    {"ThreadManForUser", 0xC0B3FFD2u, "sceKernelGetVTimerTimeWide", &GetVTimerTimeWide},
    {"ThreadManForUser", 0xB7C18B77u, "sceKernelGetVTimerBaseWide", &GetVTimerBaseWide},
    {"ThreadManForUser", 0x53B00E9Au, "sceKernelSetVTimerHandlerWide", &SetVTimerHandlerWide},
    {"ThreadManForUser", 0xD8B299AEu, "sceKernelSetVTimerHandler", &SetVTimerHandler},
    {"ThreadManForUser", 0xD2D615EFu, "sceKernelCancelVTimerHandler", &CancelVTimerHandler},
    {"ThreadManForUser", 0x3AD58B8Cu, "sceKernelSuspendDispatchThread", &SuspendDispatchThread},
    {"ThreadManForUser", 0x27E22EC2u, "sceKernelResumeDispatchThread", &ResumeDispatchThread},
    {"ModuleMgrForUser", 0x977DE386u, "sceKernelLoadModule", &LoadModule},
    {"ModuleMgrForUser", 0xB7F46618u, "sceKernelLoadModuleByID", &LoadModuleByID},
    {"ModuleMgrForUser", 0x50F0C1ECu, "sceKernelStartModule", &StartModule},
    {"ModuleMgrForUser", 0xD1FF982Au, "sceKernelStopModule", &StopModule},
    {"ModuleMgrForUser", 0x2E0911AAu, "sceKernelUnloadModule", &UnloadModule},
    {"ModuleMgrForUser", 0xF0A26395u, "sceKernelGetModuleId", &GetModuleId},
    {"ModuleMgrForUser", 0x8F2DF740u, "sceKernelStopUnloadSelfModuleWithStatus", &StopUnloadSelfModuleWithStatus},
    {"SysMemUserForUser", 0xF77D77CBu, "sceKernelSetCompilerVersion", &SetCompilerVersion},
    {"SysMemUserForUser", 0x91DE343Cu, "sceKernelSetCompiledSdkVersion500_505", &SetCompilerVersion},
    {"sceUtility", 0x2A2B3DE0u, "sceUtilityLoadModule", &UtilityLoadModule},
    {"sceUtility", 0xE49BFE92u, "sceUtilityUnloadModule", &UtilityUnloadModule},
    {"sceSuspendForUser", 0x3E0271D3u, "sceKernelVolatileMemLock", &VolatileMemLock},
    {"sceSuspendForUser", 0xA14F40B2u, "sceKernelVolatileMemTryLock", &VolatileMemLock},
    {"sceSuspendForUser", 0xA569E425u, "sceKernelVolatileMemUnlock", &VolatileMemUnlock},
    {"sceUmdUser", 0x46EBB729u, "sceUmdCheckMedium", &UmdCheckMedium},
    {"sceUmdUser", 0xC6183D47u, "sceUmdActivate", &UmdActivate},
    {"sceUmdUser", 0xE83742BAu, "sceUmdDeactivate", &UmdActivate},
    {"sceUmdUser", 0x6B4A146Cu, "sceUmdGetDriveStat", &UmdGetDriveStat},
    {"sceUmdUser", 0x8EF08FCEu, "sceUmdWaitDriveStat", &UmdWaitDriveStat},
    {"sceUmdUser", 0x56202973u, "sceUmdWaitDriveStatWithTimer", &UmdWaitDriveStat},
    {"sceUmdUser", 0x4A9E5E29u, "sceUmdWaitDriveStatCB", &UmdWaitDriveStat},
    {"sceUmdUser", 0x20628E6Fu, "sceUmdGetErrorStat", &UmdGetErrorStat},
    {"sceUmdUser", 0xAEE7404Du, "sceUmdRegisterUMDCallBack", &UmdRegisterCallback},
    {"sceUmdUser", 0xBD2BDE07u, "sceUmdUnRegisterUMDCallBack", &UmdRegisterCallback},
    {"sceUmdUser", 0x340B7686u, "sceUmdGetDiscInfo", &UmdGetDiscInfo},
    {"sceImpose", 0x36AA6E91u, "sceImposeSetLanguageMode", &ImposeSetLanguageMode},
    {"sceImpose", 0x24FD7BCFu, "sceImposeGetLanguageMode", &ImposeGetLanguageMode},
    {"sceDmac", 0x617F3FE6u, "sceDmacMemcpy", &DmacMemcpy},
    {"sceOpenPSID", 0xC69BEBCEu, "sceOpenPSIDGetOpenPSID", &GetOpenPSID},
};

} // namespace

std::span<const HleFunction> misc_functions() { return kFunctions; }

} // namespace mhf::psp::hle
