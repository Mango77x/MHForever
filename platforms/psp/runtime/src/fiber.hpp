#pragma once

// Minimal cooperative fibers: Windows fibers on Windows, ucontext elsewhere.
// Every PSP thread runs on one fiber; the scheduler runs on the host thread's own (converted) fiber.

#include <cstddef>
#include <memory>

namespace mhf::psp {

class Fiber {
public:
    using Entry = void (*)(void *argument);

    // Wraps the calling host thread so that it can switch to fibers and back. Call once per host thread.
    static std::unique_ptr<Fiber> from_current_thread();

    Fiber(std::size_t stack_size, Entry entry, void *argument);
    ~Fiber();
    Fiber(const Fiber &) = delete;
    Fiber &operator=(const Fiber &) = delete;

    // Saves the current execution state into `from` and resumes `to`.
    static void switch_to(Fiber &from, Fiber &to);

    struct Impl;

private:
    Fiber() = default;
    std::unique_ptr<Impl> impl_;
};

} // namespace mhf::psp
