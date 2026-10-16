// ThreadManForUser and Kernel_Library: threads, time, semaphores, event flags, lightweight mutexes, callbacks.

#include "hle.hpp"
#include "kernel.hpp"

#include <algorithm>
#include <cstring>

namespace mhf::psp::hle {

namespace {

using WaitType = Kernel::WaitType;
using ThreadState = Kernel::ThreadState;

Kernel &k(Runtime &rt) { return rt.kernel(); }

void write_name(Runtime &rt, std::uint32_t address, const std::string &name) {
    char buffer[32]{};
    std::strncpy(buffer, name.c_str(), sizeof buffer - 1);
    rt.memory().copy_in(address, std::span(reinterpret_cast<const std::uint8_t *>(buffer), sizeof buffer));
}

// ---------------------------------------------------------------- threads

void CreateThread(Runtime &rt, Context &ctx) {
    ret(ctx, static_cast<std::uint32_t>(k(rt).create_thread(read_string(rt, arg(ctx, 0), 32), arg(ctx, 1),
                                                            sarg(ctx, 2), arg(ctx, 3), arg(ctx, 4))));
}

void StartThread(Runtime &rt, Context &ctx) {
    ret(ctx, static_cast<std::uint32_t>(k(rt).start_thread(sarg(ctx, 0), arg(ctx, 1), arg(ctx, 2))));
}

void DeleteThread(Runtime &rt, Context &ctx) {
    ret(ctx, static_cast<std::uint32_t>(k(rt).delete_thread(sarg(ctx, 0))));
}

void ExitThread(Runtime &rt, Context &ctx) { k(rt).exit_current_thread(arg(ctx, 0), false); }
void ExitDeleteThread(Runtime &rt, Context &ctx) { k(rt).exit_current_thread(arg(ctx, 0), true); }

void TerminateThread(Runtime &rt, Context &ctx) {
    ret(ctx, static_cast<std::uint32_t>(k(rt).terminate_thread(sarg(ctx, 0))));
}

void TerminateDeleteThread(Runtime &rt, Context &ctx) {
    std::int32_t r = k(rt).terminate_thread(sarg(ctx, 0));
    if (r == 0) {
        r = k(rt).delete_thread(sarg(ctx, 0));
    }
    ret(ctx, static_cast<std::uint32_t>(r));
}

void GetThreadId(Runtime &rt, Context &ctx) { ret(ctx, static_cast<std::uint32_t>(k(rt).current()->uid)); }

std::uint32_t status_bits(ThreadState s) {
    switch (s) {
    case ThreadState::Running: return 1;
    case ThreadState::Ready: return 2;
    case ThreadState::Waiting: return 4;
    case ThreadState::Suspended: return 8;
    case ThreadState::Dormant: return 16;
    case ThreadState::Dead: return 32;
    }
    return 0;
}

void ReferThreadStatus(Runtime &rt, Context &ctx) {
    auto *t = k(rt).thread(sarg(ctx, 0));
    if (t == nullptr) {
        ret(ctx, error::kUnknownThreadId);
        return;
    }
    auto &mem = rt.memory();
    const std::uint32_t info = arg(ctx, 1);
    const std::uint32_t size = mem.load32(info);
    std::vector<std::uint8_t> out(0x68, 0);
    auto put32 = [&](std::size_t offset, std::uint32_t v) { std::memcpy(out.data() + offset, &v, 4); };
    put32(0x00, size);
    std::strncpy(reinterpret_cast<char *>(out.data() + 4), t->name.c_str(), 31);
    put32(0x24, t->attr);
    put32(0x28, status_bits(t->state));
    put32(0x2C, t->entry);
    put32(0x30, t->stack_base);
    put32(0x34, t->stack_size);
    put32(0x38, t->gp);
    put32(0x3C, static_cast<std::uint32_t>(t->initial_priority));
    put32(0x40, static_cast<std::uint32_t>(t->priority));
    put32(0x44, static_cast<std::uint32_t>(t->wait_type));
    put32(0x48, static_cast<std::uint32_t>(t->wait_object));
    put32(0x4C, static_cast<std::uint32_t>(t->wakeup_count));
    put32(0x50, t->exit_status);
    mem.copy_in(info, std::span(out.data(), std::min<std::size_t>(size, out.size())));
    ret(ctx, 0);
}

void delay(Runtime &rt, Context &ctx) {
    const std::uint64_t usec = arg(ctx, 0);
    k(rt).wait(WaitType::Delay, 0, k(rt).now_us() + usec);
    ret(ctx, 0);
}

void SleepThread(Runtime &rt, Context &ctx) {
    auto *t = k(rt).current();
    if (t->wakeup_count > 0) {
        --t->wakeup_count;
        ret(ctx, 0);
        return;
    }
    ret(ctx, k(rt).wait(WaitType::Sleep, 0, Kernel::kNoDeadline));
}

void WakeupThread(Runtime &rt, Context &ctx) {
    auto *t = k(rt).thread(sarg(ctx, 0));
    if (t == nullptr) {
        ret(ctx, error::kUnknownThreadId);
        return;
    }
    if (t->state == ThreadState::Waiting && t->wait_type == WaitType::Sleep) {
        k(rt).wake(*t, 0);
        ret(ctx, 0);
        k(rt).preempt_if_needed();
        return;
    }
    ++t->wakeup_count;
    ret(ctx, 0);
}

void CancelWakeupThread(Runtime &rt, Context &ctx) {
    auto *t = k(rt).thread(sarg(ctx, 0));
    if (t == nullptr) {
        ret(ctx, error::kUnknownThreadId);
        return;
    }
    ret(ctx, static_cast<std::uint32_t>(t->wakeup_count));
    t->wakeup_count = 0;
}

void WaitThreadEnd(Runtime &rt, Context &ctx) {
    const int uid = sarg(ctx, 0);
    auto *t = k(rt).thread(uid);
    if (t == nullptr) {
        ret(ctx, error::kUnknownThreadId);
        return;
    }
    if (t->state != ThreadState::Dormant && t->state != ThreadState::Dead) {
        const std::uint32_t timeout = arg(ctx, 1);
        const std::uint32_t r =
            k(rt).wait(WaitType::ThreadEnd, uid, k(rt).deadline_from_timeout(timeout), timeout);
        if (r != 0) {
            ret(ctx, r);
            return;
        }
        t = k(rt).thread(uid);
    }
    ret(ctx, t != nullptr ? t->exit_status : 0u);
}

void ChangeThreadPriority(Runtime &rt, Context &ctx) {
    auto *t = k(rt).thread(sarg(ctx, 0));
    if (t == nullptr) {
        ret(ctx, error::kUnknownThreadId);
        return;
    }
    int priority = sarg(ctx, 1);
    if (priority == 0) {
        priority = k(rt).current()->priority;
    }
    if (priority < 0x08 || priority > 0x77) {
        ret(ctx, error::kIllegalPriority);
        return;
    }
    t->priority = priority;
    ret(ctx, 0);
    k(rt).preempt_if_needed();
}

void GetThreadCurrentPriority(Runtime &rt, Context &ctx) {
    ret(ctx, static_cast<std::uint32_t>(k(rt).current()->priority));
}

void RotateThreadReadyQueue(Runtime &rt, Context &ctx) {
    const int priority = sarg(ctx, 0);
    ret(ctx, 0);
    if (priority == 0 || priority == k(rt).current()->priority) {
        k(rt).yield();
    }
}

void GetThreadExitStatus(Runtime &rt, Context &ctx) {
    auto *t = k(rt).thread(sarg(ctx, 0));
    if (t == nullptr) {
        ret(ctx, error::kUnknownThreadId);
        return;
    }
    ret(ctx, t->state == ThreadState::Dormant ? t->exit_status : error::kNotDormant);
}

void CheckThreadStack(Runtime &rt, Context &ctx) {
    auto *t = k(rt).current();
    ret(ctx, ctx.gpr[29] > t->stack_base ? ctx.gpr[29] - t->stack_base : 0u);
}

void GetThreadStackFreeSize(Runtime &rt, Context &ctx) {
    auto *t = k(rt).thread(sarg(ctx, 0));
    if (t == nullptr) {
        ret(ctx, error::kUnknownThreadId);
        return;
    }
    // Count the untouched 0xFF fill from the bottom of the stack.
    std::uint32_t free_bytes = 0;
    while (free_bytes < t->stack_size && rt.memory().load8(t->stack_base + free_bytes) == 0xFF) {
        ++free_bytes;
    }
    ret(ctx, free_bytes);
}

void ChangeCurrentThreadAttr(Runtime &rt, Context &ctx) {
    auto *t = k(rt).current();
    t->attr = (t->attr & ~arg(ctx, 0)) | arg(ctx, 1);
    ret(ctx, 0);
}

// ---------------------------------------------------------------- time

void GetSystemTime(Runtime &rt, Context &ctx) {
    const std::uint64_t now = k(rt).now_us();
    rt.memory().store32(arg(ctx, 0), static_cast<std::uint32_t>(now));
    rt.memory().store32(arg(ctx, 0) + 4, static_cast<std::uint32_t>(now >> 32u));
    ret(ctx, 0);
}

void GetSystemTimeWide(Runtime &rt, Context &ctx) { ret64(ctx, k(rt).now_us()); }
void GetSystemTimeLow(Runtime &rt, Context &ctx) { ret(ctx, static_cast<std::uint32_t>(k(rt).now_us())); }

void USec2SysClock(Runtime &rt, Context &ctx) {
    rt.memory().store32(arg(ctx, 1), arg(ctx, 0));
    rt.memory().store32(arg(ctx, 1) + 4, 0);
    ret(ctx, 0);
}

void USec2SysClockWide(Runtime &, Context &ctx) { ret64(ctx, arg(ctx, 0)); }

void SysClock2USec(Runtime &rt, Context &ctx) {
    const std::uint64_t clock = rt.memory().load32(arg(ctx, 0)) |
                                (static_cast<std::uint64_t>(rt.memory().load32(arg(ctx, 0) + 4)) << 32u);
    if (arg(ctx, 1) != 0) rt.memory().store32(arg(ctx, 1), static_cast<std::uint32_t>(clock / 1'000'000u));
    if (arg(ctx, 2) != 0) rt.memory().store32(arg(ctx, 2), static_cast<std::uint32_t>(clock % 1'000'000u));
    ret(ctx, 0);
}

void SysClock2USecWide(Runtime &rt, Context &ctx) {
    const std::uint64_t clock = arg(ctx, 0) | (static_cast<std::uint64_t>(arg(ctx, 1)) << 32u);
    if (arg(ctx, 2) != 0) rt.memory().store32(arg(ctx, 2), static_cast<std::uint32_t>(clock / 1'000'000u));
    if (arg(ctx, 3) != 0) rt.memory().store32(arg(ctx, 3), static_cast<std::uint32_t>(clock % 1'000'000u));
    ret(ctx, 0);
}

// ---------------------------------------------------------------- semaphores

void CreateSema(Runtime &rt, Context &ctx) {
    const int initial = sarg(ctx, 2);
    const int max = sarg(ctx, 3);
    if (initial < 0 || max <= 0 || initial > max) {
        ret(ctx, error::kIllegalCount);
        return;
    }
    Kernel::Semaphore s;
    s.uid = k(rt).next_uid();
    s.name = read_string(rt, arg(ctx, 0), 32);
    s.attr = arg(ctx, 1);
    s.initial = s.count = initial;
    s.max = max;
    const int uid = s.uid;
    k(rt).semaphores.emplace(uid, std::move(s));
    ret(ctx, static_cast<std::uint32_t>(uid));
}

void DeleteSema(Runtime &rt, Context &ctx) {
    auto it = k(rt).semaphores.find(sarg(ctx, 0));
    if (it == k(rt).semaphores.end()) {
        ret(ctx, error::kUnknownSemaId);
        return;
    }
    auto waiters = std::move(it->second.waiters);
    k(rt).semaphores.erase(it);
    k(rt).cancel_waiters(waiters, error::kWaitDelete);
    ret(ctx, 0);
    k(rt).preempt_if_needed();
}

void SignalSema(Runtime &rt, Context &ctx) {
    auto it = k(rt).semaphores.find(sarg(ctx, 0));
    if (it == k(rt).semaphores.end()) {
        ret(ctx, error::kUnknownSemaId);
        return;
    }
    auto &s = it->second;
    const int signal = sarg(ctx, 1);
    if (s.count + signal - static_cast<int>(s.waiters.size()) > s.max) {
        ret(ctx, error::kSemaOverflow);
        return;
    }
    s.count += signal;
    k(rt).signal_semaphore_waiters(s);
    ret(ctx, 0);
    k(rt).preempt_if_needed();
}

void wait_sema(Runtime &rt, Context &ctx) {
    const int uid = sarg(ctx, 0);
    auto it = k(rt).semaphores.find(uid);
    if (it == k(rt).semaphores.end()) {
        ret(ctx, error::kUnknownSemaId);
        return;
    }
    auto &s = it->second;
    const int need = sarg(ctx, 1);
    if (need <= 0 || need > s.max) {
        ret(ctx, error::kIllegalCount);
        return;
    }
    if (s.count >= need && s.waiters.empty()) {
        s.count -= need;
        ret(ctx, 0);
        return;
    }
    auto *t = k(rt).current();
    t->wait_value[0] = static_cast<std::uint32_t>(need);
    s.waiters.push_back(t->uid);
    const std::uint32_t timeout = arg(ctx, 2);
    ret(ctx, k(rt).wait(WaitType::Semaphore, uid, k(rt).deadline_from_timeout(timeout), timeout));
}

void PollSema(Runtime &rt, Context &ctx) {
    auto it = k(rt).semaphores.find(sarg(ctx, 0));
    if (it == k(rt).semaphores.end()) {
        ret(ctx, error::kUnknownSemaId);
        return;
    }
    const int need = sarg(ctx, 1);
    if (need <= 0) {
        ret(ctx, error::kIllegalCount);
        return;
    }
    if (need > it->second.count || !it->second.waiters.empty()) {
        ret(ctx, error::kSemaZero);
        return;
    }
    it->second.count -= need;
    ret(ctx, 0);
}

void ReferSemaStatus(Runtime &rt, Context &ctx) {
    auto it = k(rt).semaphores.find(sarg(ctx, 0));
    if (it == k(rt).semaphores.end()) {
        ret(ctx, error::kUnknownSemaId);
        return;
    }
    const auto &s = it->second;
    const std::uint32_t info = arg(ctx, 1);
    auto &mem = rt.memory();
    write_name(rt, info + 4, s.name);
    mem.store32(info + 36, s.attr);
    mem.store32(info + 40, static_cast<std::uint32_t>(s.initial));
    mem.store32(info + 44, static_cast<std::uint32_t>(s.count));
    mem.store32(info + 48, static_cast<std::uint32_t>(s.max));
    mem.store32(info + 52, static_cast<std::uint32_t>(s.waiters.size()));
    ret(ctx, 0);
}

// ---------------------------------------------------------------- event flags

void CreateEventFlag(Runtime &rt, Context &ctx) {
    Kernel::EventFlag f;
    f.uid = k(rt).next_uid();
    f.name = read_string(rt, arg(ctx, 0), 32);
    f.attr = arg(ctx, 1);
    f.initial = f.bits = arg(ctx, 2);
    const int uid = f.uid;
    k(rt).event_flags.emplace(uid, std::move(f));
    ret(ctx, static_cast<std::uint32_t>(uid));
}

void DeleteEventFlag(Runtime &rt, Context &ctx) {
    auto it = k(rt).event_flags.find(sarg(ctx, 0));
    if (it == k(rt).event_flags.end()) {
        ret(ctx, error::kUnknownEventFlagId);
        return;
    }
    auto waiters = std::move(it->second.waiters);
    k(rt).event_flags.erase(it);
    k(rt).cancel_waiters(waiters, error::kWaitDelete);
    ret(ctx, 0);
    k(rt).preempt_if_needed();
}

void SetEventFlag(Runtime &rt, Context &ctx) {
    auto it = k(rt).event_flags.find(sarg(ctx, 0));
    if (it == k(rt).event_flags.end()) {
        ret(ctx, error::kUnknownEventFlagId);
        return;
    }
    it->second.bits |= arg(ctx, 1);
    k(rt).signal_event_flag_waiters(it->second);
    ret(ctx, 0);
    k(rt).preempt_if_needed();
}

void ClearEventFlag(Runtime &rt, Context &ctx) {
    auto it = k(rt).event_flags.find(sarg(ctx, 0));
    if (it == k(rt).event_flags.end()) {
        ret(ctx, error::kUnknownEventFlagId);
        return;
    }
    it->second.bits &= arg(ctx, 1);
    ret(ctx, 0);
}

bool event_matches(std::uint32_t bits, std::uint32_t pattern, std::uint32_t mode) {
    return (mode & 1u) != 0 ? (bits & pattern) != 0 : (bits & pattern) == pattern;
}

// Shared by wait and poll. Returns true when the flag already satisfies the request.
bool try_event_flag(Runtime &rt, Kernel::EventFlag &f, std::uint32_t pattern, std::uint32_t mode,
                    std::uint32_t out_bits) {
    if (!event_matches(f.bits, pattern, mode)) {
        return false;
    }
    if (out_bits != 0) {
        rt.memory().store32(out_bits, f.bits);
    }
    if ((mode & 0x20u) != 0) f.bits = 0;
    else if ((mode & 0x10u) != 0) f.bits &= ~pattern;
    return true;
}

void wait_event_flag(Runtime &rt, Context &ctx) {
    const int uid = sarg(ctx, 0);
    auto it = k(rt).event_flags.find(uid);
    if (it == k(rt).event_flags.end()) {
        ret(ctx, error::kUnknownEventFlagId);
        return;
    }
    auto &f = it->second;
    const std::uint32_t pattern = arg(ctx, 1);
    const std::uint32_t mode = arg(ctx, 2);
    const std::uint32_t out_bits = arg(ctx, 3);
    if ((mode & ~0x31u) != 0 || (mode & 0x30u) == 0x30u) {
        ret(ctx, error::kIllegalMode);
        return;
    }
    if (pattern == 0) {
        ret(ctx, error::kEventFlagIllegalWaitPattern);
        return;
    }
    if (!f.waiters.empty() && (f.attr & 0x200u) == 0) {
        ret(ctx, error::kEventFlagNoMultiPermission);
        return;
    }
    if (try_event_flag(rt, f, pattern, mode, out_bits)) {
        ret(ctx, 0);
        return;
    }
    auto *t = k(rt).current();
    t->wait_value[0] = pattern;
    t->wait_value[1] = mode;
    t->wait_value[2] = out_bits;
    f.waiters.push_back(t->uid);
    const std::uint32_t timeout = arg(ctx, 4);
    const std::uint32_t r = k(rt).wait(WaitType::EventFlag, uid, k(rt).deadline_from_timeout(timeout), timeout);
    if (r != 0 && out_bits != 0) {
        if (auto again = k(rt).event_flags.find(uid); again != k(rt).event_flags.end()) {
            rt.memory().store32(out_bits, again->second.bits);
        }
    }
    ret(ctx, r);
}

void PollEventFlag(Runtime &rt, Context &ctx) {
    auto it = k(rt).event_flags.find(sarg(ctx, 0));
    if (it == k(rt).event_flags.end()) {
        ret(ctx, error::kUnknownEventFlagId);
        return;
    }
    auto &f = it->second;
    const std::uint32_t pattern = arg(ctx, 1);
    const std::uint32_t mode = arg(ctx, 2);
    const std::uint32_t out_bits = arg(ctx, 3);
    if ((mode & ~0x31u) != 0 || (mode & 0x30u) == 0x30u) {
        ret(ctx, error::kIllegalMode);
        return;
    }
    if (pattern == 0) {
        ret(ctx, error::kEventFlagIllegalWaitPattern);
        return;
    }
    if (!f.waiters.empty() && (f.attr & 0x200u) == 0) {
        ret(ctx, error::kEventFlagNoMultiPermission);
        return;
    }
    if (try_event_flag(rt, f, pattern, mode, out_bits)) {
        ret(ctx, 0);
        return;
    }
    if (out_bits != 0) {
        rt.memory().store32(out_bits, f.bits);
    }
    ret(ctx, error::kEventFlagPollFailed);
}

void ReferEventFlagStatus(Runtime &rt, Context &ctx) {
    auto it = k(rt).event_flags.find(sarg(ctx, 0));
    if (it == k(rt).event_flags.end()) {
        ret(ctx, error::kUnknownEventFlagId);
        return;
    }
    const auto &f = it->second;
    const std::uint32_t info = arg(ctx, 1);
    write_name(rt, info + 4, f.name);
    rt.memory().store32(info + 36, f.attr);
    rt.memory().store32(info + 40, f.initial);
    rt.memory().store32(info + 44, f.bits);
    rt.memory().store32(info + 48, static_cast<std::uint32_t>(f.waiters.size()));
    ret(ctx, 0);
}

// ---------------------------------------------------------------- lightweight mutexes
// Work area (guest memory): +0 lock count, +4 owner uid, +8 attr, +12 waiting threads, +16 uid.

constexpr std::uint32_t kLwMutexRecursive = 0x200u;

void CreateLwMutex(Runtime &rt, Context &ctx) {
    const std::uint32_t workarea = arg(ctx, 0);
    const std::uint32_t attr = arg(ctx, 2);
    const int count = sarg(ctx, 3);
    if (count < 0 || (count > 1 && (attr & kLwMutexRecursive) == 0)) {
        ret(ctx, error::kIllegalCount);
        return;
    }
    Kernel::LwMutex m;
    m.uid = k(rt).next_uid();
    m.name = read_string(rt, arg(ctx, 1), 32);
    m.workarea = workarea;
    auto &mem = rt.memory();
    mem.store32(workarea + 0, static_cast<std::uint32_t>(count));
    mem.store32(workarea + 4, count > 0 ? static_cast<std::uint32_t>(k(rt).current()->uid) : 0u);
    mem.store32(workarea + 8, attr);
    mem.store32(workarea + 12, 0);
    mem.store32(workarea + 16, static_cast<std::uint32_t>(m.uid));
    k(rt).lw_mutexes.emplace(m.uid, std::move(m));
    ret(ctx, 0);
}

void DeleteLwMutex(Runtime &rt, Context &ctx) {
    const std::uint32_t workarea = arg(ctx, 0);
    const int uid = static_cast<int>(rt.memory().load32(workarea + 16));
    auto it = k(rt).lw_mutexes.find(uid);
    if (it == k(rt).lw_mutexes.end()) {
        ret(ctx, error::kLwMutexNotFound);
        return;
    }
    auto waiters = std::move(it->second.waiters);
    k(rt).lw_mutexes.erase(it);
    rt.memory().store32(workarea + 16, 0);
    k(rt).cancel_waiters(waiters, error::kWaitDelete);
    ret(ctx, 0);
    k(rt).preempt_if_needed();
}

// Returns 0 when locked, an error, or 1 when the caller must wait.
std::uint32_t try_lock(Runtime &rt, std::uint32_t workarea, int count) {
    auto &mem = rt.memory();
    const int uid = static_cast<int>(mem.load32(workarea + 16));
    if (!k(rt).lw_mutexes.contains(uid)) {
        return error::kLwMutexNotFound;
    }
    if (count <= 0) {
        return error::kIllegalCount;
    }
    const auto level = static_cast<int>(mem.load32(workarea));
    const auto owner = static_cast<int>(mem.load32(workarea + 4));
    const auto self = k(rt).current()->uid;
    if (level == 0) {
        mem.store32(workarea, static_cast<std::uint32_t>(count));
        mem.store32(workarea + 4, static_cast<std::uint32_t>(self));
        return 0;
    }
    if (owner == self) {
        if ((mem.load32(workarea + 8) & kLwMutexRecursive) == 0) {
            return error::kLwMutexRecursiveNotAllowed;
        }
        mem.store32(workarea, static_cast<std::uint32_t>(level + count));
        return 0;
    }
    return 1;
}

void lock_lw_mutex(Runtime &rt, Context &ctx) {
    const std::uint32_t workarea = arg(ctx, 0);
    const int count = sarg(ctx, 1);
    const std::uint32_t r = try_lock(rt, workarea, count);
    if (r != 1) {
        ret(ctx, r);
        return;
    }
    const int uid = static_cast<int>(rt.memory().load32(workarea + 16));
    auto &m = k(rt).lw_mutexes.at(uid);
    auto *t = k(rt).current();
    t->wait_value[0] = static_cast<std::uint32_t>(count);
    m.waiters.push_back(t->uid);
    rt.memory().store32(workarea + 12, static_cast<std::uint32_t>(m.waiters.size()));
    const std::uint32_t timeout = arg(ctx, 2);
    ret(ctx, k(rt).wait(WaitType::LwMutex, uid, k(rt).deadline_from_timeout(timeout), timeout));
}

void TryLockLwMutex(Runtime &rt, Context &ctx) {
    const std::uint32_t r = try_lock(rt, arg(ctx, 0), sarg(ctx, 1));
    ret(ctx, r == 1 ? error::kLwMutexLocked : r);
}

void UnlockLwMutex(Runtime &rt, Context &ctx) {
    const std::uint32_t workarea = arg(ctx, 0);
    const int count = sarg(ctx, 1);
    auto &mem = rt.memory();
    const int uid = static_cast<int>(mem.load32(workarea + 16));
    auto it = k(rt).lw_mutexes.find(uid);
    if (it == k(rt).lw_mutexes.end()) {
        ret(ctx, error::kLwMutexNotFound);
        return;
    }
    const auto level = static_cast<int>(mem.load32(workarea));
    const auto owner = static_cast<int>(mem.load32(workarea + 4));
    if (level == 0 || owner != k(rt).current()->uid) {
        ret(ctx, error::kLwMutexUnlocked);
        return;
    }
    if (count <= 0 || level < count) {
        ret(ctx, error::kLwMutexUnlockUnderflow);
        return;
    }
    mem.store32(workarea, static_cast<std::uint32_t>(level - count));
    if (level == count) {
        mem.store32(workarea + 4, 0);
        k(rt).signal_lw_mutex_waiters(it->second);
    }
    ret(ctx, 0);
    k(rt).preempt_if_needed();
}

// ---------------------------------------------------------------- callbacks
//
// A notified callback runs on its owner thread when that thread is in a *CB wait or calls sceKernelCheckCallback:
// the CB variants of the waits run pending callbacks first, and a notification that arrives while the thread waits
// interrupts the wait (Kernel::notify_callback), runs the callbacks and waits again. A callback that returns non-zero
// is deleted.

void CreateCallback(Runtime &rt, Context &ctx) {
    Kernel::Callback c;
    c.uid = k(rt).next_uid();
    c.name = read_string(rt, arg(ctx, 0), 32);
    c.entry = arg(ctx, 1);
    c.argument = arg(ctx, 2);
    c.thread = k(rt).current()->uid;
    const int uid = c.uid;
    k(rt).callbacks.emplace(uid, std::move(c));
    ret(ctx, static_cast<std::uint32_t>(uid));
}

void DeleteCallback(Runtime &rt, Context &ctx) {
    ret(ctx, k(rt).callbacks.erase(sarg(ctx, 0)) != 0 ? 0u : error::kUnknownUid);
}

void NotifyCallback(Runtime &rt, Context &ctx) {
    auto it = k(rt).callbacks.find(sarg(ctx, 0));
    if (it == k(rt).callbacks.end()) {
        ret(ctx, error::kUnknownUid);
        return;
    }
    k(rt).notify_callback(it->second, arg(ctx, 1));
    ret(ctx, 0);
    k(rt).preempt_if_needed();
}

// Runs the current thread's notified callbacks on its own stack, preserving its registers. Returns how many ran.
int run_callbacks(Runtime &rt, Context &ctx) {
    auto *t = k(rt).current();
    if (t == nullptr) return 0;
    int ran = 0;
    for (bool again = true; again;) {
        again = false;
        for (auto it = k(rt).callbacks.begin(); it != k(rt).callbacks.end(); ++it) {
            auto &cb = it->second;
            if (cb.thread != t->uid || cb.notify_count == 0) continue;
            const int uid = cb.uid;
            const std::uint32_t count = static_cast<std::uint32_t>(cb.notify_count), notify_arg = cb.notify_arg;
            const std::uint32_t entry = cb.entry, common = cb.argument;
            cb.notify_count = 0;
            cb.notify_arg = 0;
            const Context saved = ctx;
            ctx.gpr[4] = count;
            ctx.gpr[5] = notify_arg;
            ctx.gpr[6] = common;
            ctx.gpr[29] = (ctx.gpr[29] - 64u) & ~15u;
            ctx.gpr[31] = 0;
            rt.call(ctx, entry);
            const std::uint32_t result = ctx.gpr[2];
            ctx = saved;
            ++ran;
            if (result != 0) k(rt).callbacks.erase(uid);
            again = true; // the map may have changed: start over
            break;
        }
    }
    return ran;
}

void CheckCallback(Runtime &rt, Context &ctx) { ret(ctx, run_callbacks(rt, ctx) > 0 ? 1u : 0u); }

// The *CB form of a wait: runs notified callbacks before waiting and whenever a notification interrupts the wait.
template <void (*Wait)(Runtime &, Context &)>
void with_callbacks(Runtime &rt, Context &ctx) {
    auto *t = k(rt).current();
    for (;;) {
        run_callbacks(rt, ctx);
        t->callback_wait = true;
        Wait(rt, ctx);
        t->callback_wait = false;
        if (ctx.gpr[2] != Kernel::kCallbackInterrupt) return;
    }
}

// ---------------------------------------------------------------- interrupts (cooperative runtime: no-ops)

void CpuSuspendIntr(Runtime &, Context &ctx) { ret(ctx, 1); }
void CpuResumeIntr(Runtime &, Context &ctx) { ret(ctx, 0); }
void IsCpuIntrEnable(Runtime &, Context &ctx) { ret(ctx, 1); }
void IsCpuIntrSuspended(Runtime &, Context &ctx) { ret(ctx, arg(ctx, 0) == 0 ? 1u : 0u); }

const HleFunction kFunctions[] = {
    {"ThreadManForUser", 0x446D8DE6u, "sceKernelCreateThread", &CreateThread},
    {"ThreadManForUser", 0x9FA03CD3u, "sceKernelDeleteThread", &DeleteThread},
    {"ThreadManForUser", 0xF475845Du, "sceKernelStartThread", &StartThread},
    {"ThreadManForUser", 0xAA73C935u, "sceKernelExitThread", &ExitThread},
    {"ThreadManForUser", 0x809CE29Bu, "sceKernelExitDeleteThread", &ExitDeleteThread},
    {"ThreadManForUser", 0x616403BAu, "sceKernelTerminateThread", &TerminateThread},
    {"ThreadManForUser", 0x383F7BCCu, "sceKernelTerminateDeleteThread", &TerminateDeleteThread},
    {"ThreadManForUser", 0x293B45B8u, "sceKernelGetThreadId", &GetThreadId},
    {"ThreadManForUser", 0x17C1684Eu, "sceKernelReferThreadStatus", &ReferThreadStatus},
    {"ThreadManForUser", 0xCEADEB47u, "sceKernelDelayThread", &delay},
    {"ThreadManForUser", 0x68DA9E36u, "sceKernelDelayThreadCB", &with_callbacks<&delay>},
    {"ThreadManForUser", 0x9ACE131Eu, "sceKernelSleepThread", &SleepThread},
    {"ThreadManForUser", 0x82826F70u, "sceKernelSleepThreadCB", &with_callbacks<&SleepThread>},
    {"ThreadManForUser", 0xD59EAD2Fu, "sceKernelWakeupThread", &WakeupThread},
    {"ThreadManForUser", 0xFCCFAD26u, "sceKernelCancelWakeupThread", &CancelWakeupThread},
    {"ThreadManForUser", 0x278C0DF5u, "sceKernelWaitThreadEnd", &WaitThreadEnd},
    {"ThreadManForUser", 0x840E8133u, "sceKernelWaitThreadEndCB", &with_callbacks<&WaitThreadEnd>},
    {"ThreadManForUser", 0x71BC9871u, "sceKernelChangeThreadPriority", &ChangeThreadPriority},
    {"ThreadManForUser", 0x94AA61EEu, "sceKernelGetThreadCurrentPriority", &GetThreadCurrentPriority},
    {"ThreadManForUser", 0x912354A7u, "sceKernelRotateThreadReadyQueue", &RotateThreadReadyQueue},
    {"ThreadManForUser", 0x3B183E26u, "sceKernelGetThreadExitStatus", &GetThreadExitStatus},
    {"ThreadManForUser", 0xD13BDE95u, "sceKernelCheckThreadStack", &CheckThreadStack},
    {"ThreadManForUser", 0x52089CA1u, "sceKernelGetThreadStackFreeSize", &GetThreadStackFreeSize},
    {"ThreadManForUser", 0xEA748E31u, "sceKernelChangeCurrentThreadAttr", &ChangeCurrentThreadAttr},
    {"ThreadManForUser", 0xDB738F35u, "sceKernelGetSystemTime", &GetSystemTime},
    {"ThreadManForUser", 0x82BC5777u, "sceKernelGetSystemTimeWide", &GetSystemTimeWide},
    {"ThreadManForUser", 0x369ED59Du, "sceKernelGetSystemTimeLow", &GetSystemTimeLow},
    {"ThreadManForUser", 0x110DEC9Au, "sceKernelUSec2SysClock", &USec2SysClock},
    {"ThreadManForUser", 0xC8CD158Cu, "sceKernelUSec2SysClockWide", &USec2SysClockWide},
    {"ThreadManForUser", 0xBA6B92E2u, "sceKernelSysClock2USec", &SysClock2USec},
    {"ThreadManForUser", 0xE1619D7Cu, "sceKernelSysClock2USecWide", &SysClock2USecWide},
    {"ThreadManForUser", 0xD6DA4BA1u, "sceKernelCreateSema", &CreateSema},
    {"ThreadManForUser", 0x28B6489Cu, "sceKernelDeleteSema", &DeleteSema},
    {"ThreadManForUser", 0x3F53E640u, "sceKernelSignalSema", &SignalSema},
    {"ThreadManForUser", 0x4E3A1105u, "sceKernelWaitSema", &wait_sema},
    {"ThreadManForUser", 0x6D212BACu, "sceKernelWaitSemaCB", &with_callbacks<&wait_sema>},
    {"ThreadManForUser", 0x58B1F937u, "sceKernelPollSema", &PollSema},
    {"ThreadManForUser", 0xBC6FEBC5u, "sceKernelReferSemaStatus", &ReferSemaStatus},
    {"ThreadManForUser", 0x55C20A00u, "sceKernelCreateEventFlag", &CreateEventFlag},
    {"ThreadManForUser", 0xEF9E4C70u, "sceKernelDeleteEventFlag", &DeleteEventFlag},
    {"ThreadManForUser", 0x1FB15A32u, "sceKernelSetEventFlag", &SetEventFlag},
    {"ThreadManForUser", 0x812346E4u, "sceKernelClearEventFlag", &ClearEventFlag},
    {"ThreadManForUser", 0x402FCF22u, "sceKernelWaitEventFlag", &wait_event_flag},
    {"ThreadManForUser", 0x328C546Au, "sceKernelWaitEventFlagCB", &with_callbacks<&wait_event_flag>},
    {"ThreadManForUser", 0x30FD48F0u, "sceKernelPollEventFlag", &PollEventFlag},
    {"ThreadManForUser", 0xA66B0120u, "sceKernelReferEventFlagStatus", &ReferEventFlagStatus},
    {"ThreadManForUser", 0x19CFF145u, "sceKernelCreateLwMutex", &CreateLwMutex},
    {"ThreadManForUser", 0x60107536u, "sceKernelDeleteLwMutex", &DeleteLwMutex},
    {"Kernel_Library", 0xBEA46419u, "sceKernelLockLwMutex", &lock_lw_mutex},
    {"Kernel_Library", 0x1FC64E09u, "sceKernelLockLwMutexCB", &with_callbacks<&lock_lw_mutex>},
    {"Kernel_Library", 0xDC692EE3u, "sceKernelTryLockLwMutex", &TryLockLwMutex},
    {"Kernel_Library", 0x15B6446Bu, "sceKernelUnlockLwMutex", &UnlockLwMutex},
    {"ThreadManForUser", 0xE81CAF8Fu, "sceKernelCreateCallback", &CreateCallback},
    {"ThreadManForUser", 0xEDBA5844u, "sceKernelDeleteCallback", &DeleteCallback},
    {"ThreadManForUser", 0xC11BA8C4u, "sceKernelNotifyCallback", &NotifyCallback},
    {"ThreadManForUser", 0x349D6D6Cu, "sceKernelCheckCallback", &CheckCallback},
    {"Kernel_Library", 0x092968F4u, "sceKernelCpuSuspendIntr", &CpuSuspendIntr},
    {"Kernel_Library", 0x5F10D406u, "sceKernelCpuResumeIntr", &CpuResumeIntr},
    {"Kernel_Library", 0x3B84732Du, "sceKernelCpuResumeIntrWithSync", &CpuResumeIntr},
    {"Kernel_Library", 0xB55249D2u, "sceKernelIsCpuIntrEnable", &IsCpuIntrEnable},
    {"Kernel_Library", 0x47A0B729u, "sceKernelIsCpuIntrSuspended", &IsCpuIntrSuspended},
};

} // namespace

std::span<const HleFunction> thread_functions() { return kFunctions; }

} // namespace mhf::psp::hle
