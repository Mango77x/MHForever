#include "kernel.hpp"

#include <algorithm>
#include <iterator>
#include <cstdio>
#include <thread>

namespace mhf::psp {

namespace {
constexpr std::uint32_t kThreadTopReserve = 0x100u; // per-thread area the kernel keeps at the top of each stack
constexpr std::uint32_t kAttrPriorityOrder = 0x100u;
constexpr std::uint64_t kMaxHostSleepUs = 20'000u;   // keep host_poll responsive while idle

std::uint32_t align16(std::uint32_t v) { return (v + 15u) & ~15u; }
} // namespace

Kernel::Kernel(Runtime &runtime)
    : runtime_(runtime), sysmem_(SysMem::kKernelPartitionStart + runtime.options().ram_size),
      boot_(std::chrono::steady_clock::now()) {
    sysmem_.set_next_uid(&next_uid_);
}

Kernel::~Kernel() = default;

std::uint64_t Kernel::now_us() const {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - boot_).count());
}

Kernel::Thread *Kernel::thread(int uid) {
    if (uid == 0 && current_ != nullptr) {
        return current_;
    }
    const auto it = threads_.find(uid);
    return it != threads_.end() ? it->second.get() : nullptr;
}

std::int32_t Kernel::create_thread(const std::string &name, std::uint32_t entry, int priority,
                                   std::uint32_t stack_size, std::uint32_t attr) {
    if (priority < 0x08 || priority > 0x77) {
        return static_cast<std::int32_t>(error::kIllegalPriority);
    }
    if (stack_size < 0x200u) {
        return static_cast<std::int32_t>(error::kIllegalArgument);
    }
    const auto block = sysmem_.allocate(2, "stack:" + name, SysMem::Placement::High, stack_size);
    if (!block) {
        return static_cast<std::int32_t>(error::kNoMemory);
    }
    auto t = std::make_unique<Thread>();
    t->uid = next_uid();
    t->name = name;
    t->entry = entry;
    t->priority = t->initial_priority = priority;
    t->attr = attr;
    t->stack_base = block->address;
    t->stack_size = block->size;
    t->stack_block = block->uid;
    t->gp = module_gp_;
    t->state = ThreadState::Dormant;
    // The PSP fills new stacks with 0xFF.
    runtime_.memory().zero(t->stack_base, t->stack_size);
    if (auto *p = runtime_.memory().raw_pointer(t->stack_base, t->stack_size)) {
        std::fill(p, p + t->stack_size, std::uint8_t{0xFF});
    }
    const int uid = t->uid;
    threads_.emplace(uid, std::move(t));
    return uid;
}

std::int32_t Kernel::start_thread(int uid, std::uint32_t arg_size, std::uint32_t arg_pointer) {
    Thread *t = thread(uid);
    if (t == nullptr) {
        return static_cast<std::int32_t>(error::kUnknownThreadId);
    }
    if (t->state != ThreadState::Dormant) {
        return static_cast<std::int32_t>(error::kNotDormant);
    }
    t->ctx = Context{};
    std::uint32_t sp = t->stack_base + t->stack_size - kThreadTopReserve;
    if (arg_size != 0 && arg_pointer != 0) {
        sp -= align16(arg_size);
        std::vector<std::uint8_t> args(arg_size);
        runtime_.memory().copy_out(arg_pointer, args);
        runtime_.memory().copy_in(sp, args);
        t->ctx.gpr[5] = sp;
    }
    t->ctx.gpr[4] = arg_size;
    sp -= 64u;
    t->ctx.gpr[29] = sp;
    t->ctx.gpr[28] = t->gp;
    t->ctx.gpr[26] = t->stack_base + t->stack_size - kThreadTopReserve; // $k0: thread info area
    t->ctx.gpr[31] = 0u;
    t->ctx.fcr31 = 0x00000E00u;
    // VFPU control registers at reset: identity source prefixes, no destination prefix, condition bits set.
    // The PRNG/constant words (7..15) follow PPSSPP's reset values [VERIFY on hardware].
    static constexpr std::uint32_t kVfpuCtrlReset[16] = {0xE4u, 0xE4u, 0u, 0x3Fu, 0u, 0u, 0u, 0x7772CEABu,
                                                         0x3F800001u, 0x3F800002u, 0x3F800004u, 0x3F800008u,
                                                         0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u};
    std::copy(std::begin(kVfpuCtrlReset), std::end(kVfpuCtrlReset), t->ctx.vfpu_ctrl.begin());
    t->priority = t->initial_priority;
    t->wakeup_count = 0;
    t->exit_status = error::kDormant;
    t->fiber = std::make_unique<Fiber>(runtime_.options().fiber_stack_size, &Kernel::thread_main, t);
    t->fiber_finished = false;
    t->state = ThreadState::Ready;
    t->ready_order = back_counter_++;
    if (current_ != nullptr) {
        preempt_if_needed();
    }
    return 0;
}

void Kernel::thread_main(void *argument) {
    auto *t = static_cast<Thread *>(argument);
    Runtime &rt = *Runtime::active();
    rt.call(t->ctx, t->entry);
    rt.kernel().exit_current_thread(t->ctx.gpr[2], false);
}

void Kernel::exit_current_thread(std::uint32_t status, bool delete_after) {
    Thread *t = current_;
    t->exit_status = status;
    t->state = delete_after ? ThreadState::Dead : ThreadState::Dormant;
    t->fiber_finished = true;
    // Wake threads waiting for this one to end.
    for (auto &[uid, other] : threads_) {
        if (other->state == ThreadState::Waiting && other->wait_type == WaitType::ThreadEnd &&
            other->wait_object == t->uid) {
            wake(*other, 0);
        }
    }
    switch_to_scheduler();
    std::abort(); // a finished thread is never resumed
}

std::int32_t Kernel::terminate_thread(int uid) {
    Thread *t = thread(uid);
    if (t == nullptr) {
        return static_cast<std::int32_t>(error::kUnknownThreadId);
    }
    if (t == current_) {
        return static_cast<std::int32_t>(error::kIllegalThreadId);
    }
    if (t->state == ThreadState::Dormant || t->state == ThreadState::Dead) {
        return static_cast<std::int32_t>(error::kDormant);
    }
    remove_from_wait_queue(*t);
    t->state = ThreadState::Dormant;
    t->exit_status = 0x800201ACu; // SCE_KERNEL_ERROR_THREAD_TERMINATED
    t->fiber.reset();              // not running: safe to drop its stack
    for (auto &[other_uid, other] : threads_) {
        if (other->state == ThreadState::Waiting && other->wait_type == WaitType::ThreadEnd &&
            other->wait_object == uid) {
            wake(*other, 0);
        }
    }
    return 0;
}

std::int32_t Kernel::delete_thread(int uid) {
    Thread *t = thread(uid);
    if (t == nullptr) {
        return static_cast<std::int32_t>(error::kUnknownThreadId);
    }
    if (t->state != ThreadState::Dormant && t->state != ThreadState::Dead) {
        return static_cast<std::int32_t>(error::kNotDormant);
    }
    if (t == current_) {
        return static_cast<std::int32_t>(error::kIllegalThreadId);
    }
    sysmem_.free(t->stack_block);
    threads_.erase(uid);
    return 0;
}

std::uint64_t Kernel::deadline_from_timeout(std::uint32_t timeout_address) const {
    if (timeout_address == 0) {
        return kNoDeadline;
    }
    return now_us() + runtime_.memory().load32(timeout_address);
}

std::uint32_t Kernel::wait(WaitType type, int object, std::uint64_t deadline, std::uint32_t timeout_address) {
    Thread *t = current_;
    t->state = ThreadState::Waiting;
    t->wait_type = type;
    t->wait_object = object;
    t->wait_deadline = deadline;
    t->wait_timeout_address = timeout_address;
    t->wait_result = 0;
    switch_to_scheduler();
    // Resumed: report the remaining time when the guest gave a timeout.
    if (timeout_address != 0 && deadline != kNoDeadline) {
        const std::uint64_t now = now_us();
        runtime_.memory().store32(timeout_address,
                                  now >= deadline ? 0u : static_cast<std::uint32_t>(deadline - now));
    }
    return t->wait_result;
}

void Kernel::wake(Thread &t, std::uint32_t result) {
    if (t.state != ThreadState::Waiting) {
        return;
    }
    t.wait_result = result;
    t.wait_type = WaitType::None;
    t.wait_deadline = kNoDeadline;
    t.state = ThreadState::Ready;
    t.ready_order = back_counter_++;
}

void Kernel::preempt_if_needed() {
    Thread *t = current_;
    if (t == nullptr) {
        return;
    }
    for (const auto &[uid, other] : threads_) {
        if (other->state == ThreadState::Ready && other->priority < t->priority) {
            t->state = ThreadState::Ready;
            t->ready_order = --front_counter_; // a preempted thread resumes first among its priority
            switch_to_scheduler();
            return;
        }
    }
}

void Kernel::yield() {
    Thread *t = current_;
    t->state = ThreadState::Ready;
    t->ready_order = back_counter_++;
    switch_to_scheduler();
}

void Kernel::switch_to_scheduler() {
    Thread *t = current_;
    Fiber::switch_to(*t->fiber, *scheduler_fiber_);
}

void Kernel::remove_from_wait_queue(Thread &t) {
    auto erase = [&](std::deque<int> &q) { q.erase(std::remove(q.begin(), q.end(), t.uid), q.end()); };
    switch (t.wait_type) {
    case WaitType::Semaphore:
        if (auto it = semaphores.find(t.wait_object); it != semaphores.end()) erase(it->second.waiters);
        break;
    case WaitType::EventFlag:
        if (auto it = event_flags.find(t.wait_object); it != event_flags.end()) erase(it->second.waiters);
        break;
    case WaitType::LwMutex:
        if (auto it = lw_mutexes.find(t.wait_object); it != lw_mutexes.end()) erase(it->second.waiters);
        break;
    default:
        break;
    }
}

namespace {
// Orders a wait queue by priority when the object asks for it (attr 0x100); FIFO otherwise.
template <typename Lookup>
std::vector<int> wake_order(const std::deque<int> &waiters, std::uint32_t attr, Lookup &&lookup) {
    std::vector<int> order(waiters.begin(), waiters.end());
    if ((attr & kAttrPriorityOrder) != 0) {
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return lookup(a)->priority < lookup(b)->priority;
        });
    }
    return order;
}
} // namespace

void Kernel::signal_semaphore_waiters(Semaphore &sema) {
    for (const int uid : wake_order(sema.waiters, sema.attr, [&](int u) { return thread(u); })) {
        Thread *t = thread(uid);
        if (t == nullptr) {
            continue;
        }
        const int need = static_cast<int>(t->wait_value[0]);
        if (need <= sema.count) {
            sema.count -= need;
            sema.waiters.erase(std::find(sema.waiters.begin(), sema.waiters.end(), uid));
            wake(*t, 0);
        }
    }
}

namespace {
bool event_matches(std::uint32_t bits, std::uint32_t pattern, std::uint32_t mode) {
    return (mode & 1u) != 0 ? (bits & pattern) != 0 : (bits & pattern) == pattern; // 1 = OR, 0 = AND
}
} // namespace

void Kernel::signal_event_flag_waiters(EventFlag &flag) {
    for (const int uid : wake_order(flag.waiters, flag.attr, [&](int u) { return thread(u); })) {
        Thread *t = thread(uid);
        if (t == nullptr) {
            continue;
        }
        const std::uint32_t pattern = t->wait_value[0];
        const std::uint32_t mode = t->wait_value[1];
        if (event_matches(flag.bits, pattern, mode)) {
            if (t->wait_value[2] != 0) {
                runtime_.memory().store32(t->wait_value[2], flag.bits);
            }
            if ((mode & 0x20u) != 0) flag.bits = 0;          // CLEARALL
            else if ((mode & 0x10u) != 0) flag.bits &= ~pattern; // CLEAR
            flag.waiters.erase(std::find(flag.waiters.begin(), flag.waiters.end(), uid));
            wake(*t, 0);
        }
    }
}

void Kernel::signal_lw_mutex_waiters(LwMutex &mutex) {
    auto &mem = runtime_.memory();
    if (mutex.waiters.empty() || mem.load32(mutex.workarea) != 0) {
        return;
    }
    const int uid = mutex.waiters.front();
    mutex.waiters.pop_front();
    Thread *t = thread(uid);
    if (t == nullptr) {
        return;
    }
    mem.store32(mutex.workarea + 0, t->wait_value[0]);
    mem.store32(mutex.workarea + 4, static_cast<std::uint32_t>(uid));
    mem.store32(mutex.workarea + 12, static_cast<std::uint32_t>(mutex.waiters.size()));
    wake(*t, 0);
}

void Kernel::cancel_waiters(std::deque<int> &waiters, std::uint32_t result) {
    for (const int uid : waiters) {
        if (Thread *t = thread(uid)) {
            wake(*t, result);
        }
    }
    waiters.clear();
}

Kernel::Thread *Kernel::pick_ready() {
    Thread *best = nullptr;
    for (const auto &[uid, t] : threads_) {
        if (t->state != ThreadState::Ready) {
            continue;
        }
        if (best == nullptr || t->priority < best->priority ||
            (t->priority == best->priority && t->ready_order < best->ready_order)) {
            best = t.get();
        }
    }
    return best;
}

void Kernel::wake_timeouts() {
    const std::uint64_t now = now_us();
    for (auto &[uid, t] : threads_) {
        if (t->state == ThreadState::Waiting && t->wait_deadline != kNoDeadline && t->wait_deadline <= now) {
            const bool timed_wait = t->wait_type != WaitType::Delay && t->wait_type != WaitType::Vblank;
            remove_from_wait_queue(*t);
            wake(*t, timed_wait ? error::kWaitTimeout : 0u);
        }
    }
}

void Kernel::destroy_finished_fibers() {
    for (auto it = threads_.begin(); it != threads_.end();) {
        Thread &t = *it->second;
        if (t.fiber_finished && t.fiber) {
            t.fiber.reset();
        }
        if (t.state == ThreadState::Dead && !t.fiber) {
            sysmem_.free(t.stack_block);
            it = threads_.erase(it);
        } else {
            ++it;
        }
    }
}

int Kernel::run_scheduler() {
    scheduler_fiber_ = Fiber::from_current_thread();
    while (!exit_code_) {
        if (host_poll) {
            host_poll();
        }
        wake_timeouts();
        Thread *next = pick_ready();
        if (next == nullptr) {
            std::uint64_t earliest = kNoDeadline;
            bool any_alive = false;
            for (const auto &[uid, t] : threads_) {
                if (t->state == ThreadState::Waiting) {
                    any_alive = true;
                    earliest = std::min(earliest, t->wait_deadline);
                }
            }
            if (!any_alive) {
                std::fprintf(stderr, "[kernel] no threads left to run; stopping\n");
                return 0;
            }
            if (earliest == kNoDeadline && !host_poll) {
                std::fprintf(stderr, "[kernel] deadlock: every thread waits forever\n");
                for (const auto &[uid, t] : threads_) {
                    if (t->state == ThreadState::Waiting) {
                        std::fprintf(stderr, "[kernel]   thread %d '%s' wait type %d object %d\n", uid,
                                     t->name.c_str(), static_cast<int>(t->wait_type), t->wait_object);
                    }
                }
                return 1;
            }
            const std::uint64_t now = now_us();
            const std::uint64_t sleep = earliest == kNoDeadline ? kMaxHostSleepUs
                                                                : std::min(earliest > now ? earliest - now : 0,
                                                                           kMaxHostSleepUs);
            if (sleep > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(sleep));
            }
            continue;
        }
        current_ = next;
        next->state = ThreadState::Running;
        Fiber::switch_to(*scheduler_fiber_, *next->fiber);
        current_ = nullptr;
        destroy_finished_fibers();
    }
    return *exit_code_;
}

void Kernel::exit_program(int code) {
    exit_code_ = code;
    if (current_ != nullptr) {
        current_->fiber_finished = true;
        Fiber::switch_to(*current_->fiber, *scheduler_fiber_);
    }
    std::abort(); // never resumed
}

} // namespace mhf::psp
