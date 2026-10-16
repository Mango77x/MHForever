#pragma once

// PSP kernel objects and the thread scheduler (docs/05-runtime-hle.md §2).
//
// Scheduling follows the PSP: strict priority (lower value runs first), FIFO among equal priorities, a thread runs
// until it blocks, yields, exits or a higher-priority thread becomes ready. Each thread is a host fiber; the scheduler
// loop runs on the host thread's own fiber.

#include "fiber.hpp"
#include "sysmem.hpp"

#include "mhf/psp/runtime.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <initializer_list>
#include <optional>
#include <string>

namespace mhf::psp {

class Kernel {
public:
    static constexpr std::uint64_t kNoDeadline = ~0ull;

    enum class ThreadState { Ready, Running, Waiting, Suspended, Dormant, Dead };
    enum class WaitType { None, Delay, Sleep, Semaphore, EventFlag, LwMutex, ThreadEnd, Vblank, Generic };

    struct Thread {
        int uid{};
        std::string name;
        std::uint32_t entry{};
        int priority{};
        int initial_priority{};
        std::uint32_t attr{};
        std::uint32_t stack_base{};
        std::uint32_t stack_size{};
        int stack_block{};
        std::uint32_t gp{};
        Context ctx{};
        std::unique_ptr<Fiber> fiber;
        bool fiber_finished{};
        ThreadState state{ThreadState::Dormant};
        std::int64_t ready_order{};
        // Wait bookkeeping.
        WaitType wait_type{WaitType::None};
        int wait_object{};
        std::uint64_t wait_deadline{kNoDeadline};
        std::uint32_t wait_timeout_address{};
        std::uint32_t wait_value[3]{};
        std::uint32_t wait_result{};
        int wakeup_count{};
        bool callback_wait{}; // blocked in a *CB wait: notified callbacks interrupt it to run
        std::uint32_t exit_status{error::kDormant};
    };

    struct Semaphore {
        int uid{};
        std::string name;
        std::uint32_t attr{};
        int initial{};
        int count{};
        int max{};
        std::deque<int> waiters;
    };

    struct EventFlag {
        int uid{};
        std::string name;
        std::uint32_t attr{};
        std::uint32_t initial{};
        std::uint32_t bits{};
        std::deque<int> waiters;
    };

    struct LwMutex {
        int uid{};
        std::string name;
        std::uint32_t workarea{};
        std::deque<int> waiters;
    };

    struct Callback {
        int uid{};
        std::string name;
        int thread{};
        std::uint32_t entry{};
        std::uint32_t argument{};
        int notify_count{};
        std::uint32_t notify_arg{};
    };

    explicit Kernel(Runtime &runtime);
    ~Kernel();

    // Time since boot in microseconds (host monotonic clock).
    [[nodiscard]] std::uint64_t now_us() const;

    [[nodiscard]] SysMem &sysmem() noexcept { return sysmem_; }
    [[nodiscard]] int next_uid() noexcept { return next_uid_++; }

    void set_module_gp(std::uint32_t gp) noexcept { module_gp_ = gp; }

    // Threads.
    [[nodiscard]] Thread *current() noexcept { return current_; }
    [[nodiscard]] Thread *thread(int uid);
    std::int32_t create_thread(const std::string &name, std::uint32_t entry, int priority, std::uint32_t stack_size,
                               std::uint32_t attr);
    std::int32_t start_thread(int uid, std::uint32_t arg_size, std::uint32_t arg_pointer);
    std::int32_t delete_thread(int uid);
    std::int32_t terminate_thread(int uid);
    [[noreturn]] void exit_current_thread(std::uint32_t status, bool delete_after);

    // Blocks the current thread until woken. Returns the wait result (0, timeout, delete, ...).
    // `timeout_address`, when non-zero, holds the timeout in microseconds and receives the remaining time.
    std::uint32_t wait(WaitType type, int object, std::uint64_t deadline, std::uint32_t timeout_address = 0);
    [[nodiscard]] std::uint64_t deadline_from_timeout(std::uint32_t timeout_address) const;
    void wake(Thread &thread, std::uint32_t result);
    // Lets a higher-priority ready thread run (call after waking threads or changing priorities).
    void preempt_if_needed();
    // Moves the current thread to the back of its priority's ready queue and switches.
    void yield();

    // Synchronization objects.
    std::map<int, Semaphore> semaphores;
    std::map<int, EventFlag> event_flags;
    std::map<int, LwMutex> lw_mutexes;
    std::map<int, Callback> callbacks;
    // Wait result used internally to interrupt a *CB wait so that its thread runs its notified callbacks.
    static constexpr std::uint32_t kCallbackInterrupt = 0x7FFFCB01u;
    // Records a notification; when the owner thread sits in a *CB wait, interrupts that wait.
    void notify_callback(Callback &callback, std::uint32_t argument);
    int exit_callback{};

    // Interrupt handlers. Only the vblank interrupt (30) is delivered; its sub-handlers run at every vblank.
    struct SubInterrupt {
        std::uint32_t handler{};
        std::uint32_t argument{};
        bool enabled{};
    };
    static constexpr int kVblankInterrupt = 30;
    std::map<int, SubInterrupt> vblank_handlers; // by sub-interrupt number

    // Virtual timers: count microseconds while running and call their handler at a scheduled time.
    struct VTimer {
        int uid{};
        std::string name;
        bool running{};
        std::uint64_t started_at{};   // host time when last started
        std::uint64_t accumulated{};  // time counted before the last start
        std::uint64_t schedule{};     // timer time at which the handler fires
        std::uint32_t handler{};
        std::uint32_t common{};
    };
    std::map<int, VTimer> vtimers;
    [[nodiscard]] std::uint64_t vtimer_time(const VTimer &timer) const;

    // Runs a guest function in interrupt context (no current thread, its own stack). Returns $v0.
    std::uint32_t call_from_interrupt(std::uint32_t function, std::initializer_list<std::uint32_t> args);
    // Scratch guest memory owned by the interrupt context (for structures passed to handlers).
    [[nodiscard]] std::uint32_t interrupt_scratch();

    void signal_semaphore_waiters(Semaphore &sema);
    void signal_event_flag_waiters(EventFlag &flag);
    void signal_lw_mutex_waiters(LwMutex &mutex);
    void cancel_waiters(std::deque<int> &waiters, std::uint32_t result);

    // Prints every thread with its state, wait and registers of interest (debugging hangs).
    void dump_threads(std::FILE *out) const;
    // Runs the scheduler until exit. Returns the program exit code.
    int run_scheduler();
    [[noreturn]] void exit_program(int code);
    // Asks the scheduler to stop at its next iteration (safe from host hooks).
    void request_exit(int code) { exit_code_ = code; }
    [[nodiscard]] bool exiting() const noexcept { return exit_code_.has_value(); }

    // Host hook called by the scheduler on every iteration (input, presentation). May be empty.
    std::function<void()> host_poll;

    [[nodiscard]] const std::map<int, std::unique_ptr<Thread>> &threads() const noexcept { return threads_; }

private:
    static void thread_main(void *argument);
    void switch_to_scheduler();
    void remove_from_wait_queue(Thread &thread);
    [[nodiscard]] Thread *pick_ready();
    void wake_timeouts();
    void destroy_finished_fibers();
    void service_interrupts();
    [[nodiscard]] std::uint64_t next_interrupt_time() const;
    void ensure_interrupt_context();

    Runtime &runtime_;
    SysMem sysmem_;
    int next_uid_{0x1000};
    std::uint32_t module_gp_{};
    std::chrono::steady_clock::time_point boot_;
    std::map<int, std::unique_ptr<Thread>> threads_;
    std::unique_ptr<Fiber> scheduler_fiber_;
    Thread *current_{};
    std::int64_t back_counter_{0};
    std::int64_t front_counter_{0};
    std::optional<int> exit_code_;
    Context interrupt_ctx_{};
    std::uint32_t interrupt_stack_top_{};
    std::uint64_t last_vblank_{};
    bool in_interrupt_{};
};

} // namespace mhf::psp
