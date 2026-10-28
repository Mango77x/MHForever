// LoadExecForUser, ModuleMgrForUser, UtilsForUser, sceRtc, scePower, sceSuspendForUser and system parameters.

#include "hle.hpp"
#include "kernel.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

namespace mhf::psp::hle {

namespace {

// ---------------------------------------------------------------- LoadExecForUser / ModuleMgrForUser

void ExitGame(Runtime &rt, Context &) { rt.exit_program(0); }
void ExitGameWithStatus(Runtime &rt, Context &ctx) { rt.exit_program(sarg(ctx, 0)); }

void RegisterExitCallback(Runtime &rt, Context &ctx) {
    rt.kernel().exit_callback = sarg(ctx, 0);
    ret(ctx, 0);
}

void SelfStopUnloadModule(Runtime &rt, Context &ctx) { rt.exit_program(sarg(ctx, 1)); }
void GetModuleIdByAddress(Runtime &, Context &ctx) { ret(ctx, 0x1001u); }

// ---------------------------------------------------------------- UtilsForUser

std::uint64_t unix_microseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

void LibcTime(Runtime &rt, Context &ctx) {
    const auto t = static_cast<std::uint32_t>(unix_microseconds() / 1'000'000u);
    if (arg(ctx, 0) != 0) rt.memory().store32(arg(ctx, 0), t);
    ret(ctx, t);
}

void LibcClock(Runtime &rt, Context &ctx) { ret(ctx, static_cast<std::uint32_t>(rt.kernel().now_us())); }

void LibcGettimeofday(Runtime &rt, Context &ctx) {
    const std::uint64_t us = unix_microseconds();
    if (arg(ctx, 0) != 0) {
        rt.memory().store32(arg(ctx, 0), static_cast<std::uint32_t>(us / 1'000'000u));
        rt.memory().store32(arg(ctx, 0) + 4, static_cast<std::uint32_t>(us % 1'000'000u));
    }
    if (arg(ctx, 1) != 0) {
        rt.memory().store32(arg(ctx, 1), 0);
        rt.memory().store32(arg(ctx, 1) + 4, 0);
    }
    ret(ctx, 0);
}

void CacheOp(Runtime &, Context &ctx) { ret(ctx, 0); } // host memory is coherent

// Mersenne Twister state lives in the guest's buffer; we keep a host copy keyed by its address.
void Mt19937Init(Runtime &rt, Context &ctx) {
    // Store the 624-word state in the guest context (the PSP structure is 625 words).
    std::array<std::uint32_t, 624> state{};
    std::uint32_t x = arg(ctx, 1);
    state[0] = x;
    for (std::uint32_t i = 1; i < 624; ++i) {
        x = 1812433253u * (x ^ (x >> 30u)) + i;
        state[i] = x;
    }
    const std::uint32_t base = arg(ctx, 0);
    for (std::uint32_t i = 0; i < 624; ++i) rt.memory().store32(base + i * 4, state[i]);
    rt.memory().store32(base + 624 * 4, 624);
    ret(ctx, 0);
}

void Mt19937UInt(Runtime &rt, Context &ctx) {
    const std::uint32_t base = arg(ctx, 0);
    auto &mem = rt.memory();
    std::uint32_t index = mem.load32(base + 624 * 4);
    if (index >= 624) {
        for (std::uint32_t i = 0; i < 624; ++i) {
            const std::uint32_t y = (mem.load32(base + i * 4) & 0x80000000u) |
                                    (mem.load32(base + ((i + 1) % 624) * 4) & 0x7FFFFFFFu);
            std::uint32_t v = mem.load32(base + ((i + 397) % 624) * 4) ^ (y >> 1u);
            if ((y & 1u) != 0) v ^= 0x9908B0DFu;
            mem.store32(base + i * 4, v);
        }
        index = 0;
    }
    std::uint32_t y = mem.load32(base + index * 4);
    mem.store32(base + 624 * 4, index + 1);
    y ^= y >> 11u;
    y ^= (y << 7u) & 0x9D2C5680u;
    y ^= (y << 15u) & 0xEFC60000u;
    y ^= y >> 18u;
    ret(ctx, y);
}

// ---------------------------------------------------------------- sceRtc
// Ticks are microseconds since 0001-01-01.

constexpr std::uint64_t kUnixEpochTicks = 62135596800000000ull;

std::uint64_t current_tick() { return unix_microseconds() + kUnixEpochTicks; }

void RtcGetCurrentTick(Runtime &rt, Context &ctx) {
    const std::uint64_t tick = current_tick();
    rt.memory().store32(arg(ctx, 0), static_cast<std::uint32_t>(tick));
    rt.memory().store32(arg(ctx, 0) + 4, static_cast<std::uint32_t>(tick >> 32u));
    ret(ctx, 0);
}

void RtcGetTickResolution(Runtime &, Context &ctx) { ret(ctx, 1'000'000u); }

// ScePspDateTime: u16 year, month, day, hour, minute, second; u32 microsecond.
void write_datetime(Runtime &rt, std::uint32_t address, std::uint64_t unix_us, bool local) {
    const auto t = static_cast<std::time_t>(unix_us / 1'000'000u);
    std::tm tm{};
#if defined(_WIN32)
    if (local) localtime_s(&tm, &t); else gmtime_s(&tm, &t);
#else
    if (local) localtime_r(&t, &tm); else gmtime_r(&t, &tm);
#endif
    auto &mem = rt.memory();
    mem.store16(address + 0, static_cast<std::uint16_t>(tm.tm_year + 1900));
    mem.store16(address + 2, static_cast<std::uint16_t>(tm.tm_mon + 1));
    mem.store16(address + 4, static_cast<std::uint16_t>(tm.tm_mday));
    mem.store16(address + 6, static_cast<std::uint16_t>(tm.tm_hour));
    mem.store16(address + 8, static_cast<std::uint16_t>(tm.tm_min));
    mem.store16(address + 10, static_cast<std::uint16_t>(tm.tm_sec));
    mem.store32(address + 12, static_cast<std::uint32_t>(unix_us % 1'000'000u));
}

void RtcGetCurrentClock(Runtime &rt, Context &ctx) {
    // a1 = timezone offset in minutes.
    const std::int64_t offset = static_cast<std::int64_t>(sarg(ctx, 1)) * 60'000'000;
    write_datetime(rt, arg(ctx, 0), static_cast<std::uint64_t>(static_cast<std::int64_t>(unix_microseconds()) + offset),
                   false);
    ret(ctx, 0);
}

void RtcGetCurrentClockLocalTime(Runtime &rt, Context &ctx) {
    write_datetime(rt, arg(ctx, 0), unix_microseconds(), true);
    ret(ctx, 0);
}

void RtcSetTick(Runtime &rt, Context &ctx) {
    const std::uint64_t tick = rt.memory().load32(arg(ctx, 1)) |
                               (static_cast<std::uint64_t>(rt.memory().load32(arg(ctx, 1) + 4)) << 32u);
    write_datetime(rt, arg(ctx, 0), tick >= kUnixEpochTicks ? tick - kUnixEpochTicks : 0, false);
    ret(ctx, 0);
}

// ---------------------------------------------------------------- scePower / sceSuspendForUser

int g_cpu_mhz = 222;
int g_bus_mhz = 111;

void PowerSetClockFrequency(Runtime &, Context &ctx) {
    g_cpu_mhz = sarg(ctx, 1);
    g_bus_mhz = sarg(ctx, 2);
    ret(ctx, 0);
}
void PowerSetCpuClockFrequency(Runtime &, Context &ctx) { g_cpu_mhz = sarg(ctx, 0); ret(ctx, 0); }
void PowerSetBusClockFrequency(Runtime &, Context &ctx) { g_bus_mhz = sarg(ctx, 0); ret(ctx, 0); }
void PowerGetCpuClockFrequency(Runtime &, Context &ctx) { ret(ctx, static_cast<std::uint32_t>(g_cpu_mhz)); }
void PowerGetBusClockFrequency(Runtime &, Context &ctx) { ret(ctx, static_cast<std::uint32_t>(g_bus_mhz)); }
void PowerBatteryPercent(Runtime &, Context &ctx) { ret(ctx, 100); }
void PowerIsPowerOnline(Runtime &, Context &ctx) { ret(ctx, 1); }
void PowerZero(Runtime &, Context &ctx) { ret(ctx, 0); }
void PowerRegisterCallback(Runtime &, Context &ctx) { ret(ctx, 0); }

// ---------------------------------------------------------------- sceUtility system parameters

void UtilityGetSystemParamInt(Runtime &rt, Context &ctx) {
    std::uint32_t value = 0;
    switch (arg(ctx, 0)) {
    case 1: value = 0; break;  // adhoc channel: automatic
    case 2: value = 0; break;  // WLAN power save
    case 3: value = 0; break;  // date format YYYYMMDD
    case 4: value = 1; break;  // time format 24h
    case 5: value = 60; break; // timezone (minutes)
    case 6: value = 0; break;  // daylight saving
    case 8:                    // language: what the player chose (English unless they picked another)
        value = static_cast<std::uint32_t>(std::clamp(rt.options().system_language, 0, 5));
        break;
    case 9: value = 1; break;  // confirm button: cross
    default: ret(ctx, 0x80110103u); return; // invalid id
    }
    rt.memory().store32(arg(ctx, 1), value);
    ret(ctx, 0);
}

void UtilityGetSystemParamString(Runtime &rt, Context &ctx) {
    if (arg(ctx, 0) == 1) { // nickname
        const char name[] = "Hunter";
        rt.memory().copy_in(arg(ctx, 1), std::span(reinterpret_cast<const std::uint8_t *>(name), sizeof name));
        ret(ctx, 0);
        return;
    }
    ret(ctx, 0x80110103u);
}

const HleFunction kFunctions[] = {
    {"LoadExecForUser", 0x05572A5Fu, "sceKernelExitGame", &ExitGame},
    {"LoadExecForUser", 0x2AC9954Bu, "sceKernelExitGameWithStatus", &ExitGameWithStatus},
    {"LoadExecForUser", 0x4AC57943u, "sceKernelRegisterExitCallback", &RegisterExitCallback},
    {"ModuleMgrForUser", 0xD675EBB8u, "sceKernelSelfStopUnloadModule", &SelfStopUnloadModule},
    {"ModuleMgrForUser", 0xCC1D3699u, "sceKernelStopUnloadSelfModule", &SelfStopUnloadModule},
    {"ModuleMgrForUser", 0xD8B73127u, "sceKernelGetModuleIdByAddress", &GetModuleIdByAddress},
    {"UtilsForUser", 0x27CC57F0u, "sceKernelLibcTime", &LibcTime},
    {"UtilsForUser", 0x91E4F6A7u, "sceKernelLibcClock", &LibcClock},
    {"UtilsForUser", 0x71EC4271u, "sceKernelLibcGettimeofday", &LibcGettimeofday},
    {"UtilsForUser", 0x79D1C3FAu, "sceKernelDcacheWritebackAll", &CacheOp},
    {"UtilsForUser", 0x3EE30821u, "sceKernelDcacheWritebackRange", &CacheOp},
    {"UtilsForUser", 0xB435DEC5u, "sceKernelDcacheWritebackInvalidateAll", &CacheOp},
    {"UtilsForUser", 0x34B9FA9Eu, "sceKernelDcacheWritebackInvalidateRange", &CacheOp},
    {"UtilsForUser", 0xBFA98062u, "sceKernelDcacheInvalidateRange", &CacheOp},
    {"UtilsForUser", 0x920F104Au, "sceKernelIcacheInvalidateAll", &CacheOp},
    {"UtilsForUser", 0xC2DF770Eu, "sceKernelIcacheInvalidateRange", &CacheOp},
    {"UtilsForUser", 0xE860E75Eu, "sceKernelUtilsMt19937Init", &Mt19937Init},
    {"UtilsForUser", 0x06FB8A63u, "sceKernelUtilsMt19937UInt", &Mt19937UInt},
    {"sceRtc", 0x3F7AD767u, "sceRtcGetCurrentTick", &RtcGetCurrentTick},
    {"sceRtc", 0xC41C2853u, "sceRtcGetTickResolution", &RtcGetTickResolution},
    {"sceRtc", 0x4CFA57B0u, "sceRtcGetCurrentClock", &RtcGetCurrentClock},
    {"sceRtc", 0xE7C27D1Bu, "sceRtcGetCurrentClockLocalTime", &RtcGetCurrentClockLocalTime},
    {"sceRtc", 0x7ED29E40u, "sceRtcSetTick", &RtcSetTick},
    {"scePower", 0x737486F2u, "scePowerSetClockFrequency", &PowerSetClockFrequency},
    {"scePower", 0x843FBF43u, "scePowerSetCpuClockFrequency", &PowerSetCpuClockFrequency},
    {"scePower", 0xB8D7B3FBu, "scePowerSetBusClockFrequency", &PowerSetBusClockFrequency},
    {"scePower", 0xFEE03A2Fu, "scePowerGetCpuClockFrequency", &PowerGetCpuClockFrequency},
    {"scePower", 0xFDB5BFE9u, "scePowerGetCpuClockFrequencyInt", &PowerGetCpuClockFrequency},
    {"scePower", 0x478FE6F5u, "scePowerGetBusClockFrequency", &PowerGetBusClockFrequency},
    {"scePower", 0xBD681969u, "scePowerGetBusClockFrequencyInt", &PowerGetBusClockFrequency},
    {"scePower", 0x2085D15Du, "scePowerGetBatteryLifePercent", &PowerBatteryPercent},
    {"scePower", 0x87440F5Eu, "scePowerIsPowerOnline", &PowerIsPowerOnline},
    {"scePower", 0x0AFD0D8Bu, "scePowerIsBatteryExist", &PowerIsPowerOnline},
    {"scePower", 0x1E490401u, "scePowerIsBatteryCharging", &PowerZero},
    {"scePower", 0xD3075926u, "scePowerIsLowBattery", &PowerZero},
    {"scePower", 0x04B7766Eu, "scePowerRegisterCallback", &PowerRegisterCallback},
    {"scePower", 0xDFA8BAF8u, "scePowerUnregisterCallback", &PowerZero},
    {"scePower", 0xD6D016EFu, "scePowerLock", &PowerZero},
    {"scePower", 0xCA3D34C1u, "scePowerUnlock", &PowerZero},
    {"scePower", 0xEFD3C963u, "scePowerTick", &PowerZero},
    {"scePower", 0x7F30B3B1u, "scePowerIdleTimerEnable", &PowerZero},
    {"scePower", 0x972CE941u, "scePowerIdleTimerDisable", &PowerZero},
    {"sceSuspendForUser", 0xEADB1BD7u, "sceKernelPowerLock", &PowerZero},
    {"sceSuspendForUser", 0x3AEE7261u, "sceKernelPowerUnlock", &PowerZero},
    {"sceSuspendForUser", 0x090CCB3Fu, "sceKernelPowerTick", &PowerZero},
    {"sceUtility", 0xA5DA2406u, "sceUtilityGetSystemParamInt", &UtilityGetSystemParamInt},
    {"sceUtility", 0x34B78343u, "sceUtilityGetSystemParamString", &UtilityGetSystemParamString},
};

} // namespace

std::span<const HleFunction> system_functions() { return kFunctions; }

} // namespace mhf::psp::hle
