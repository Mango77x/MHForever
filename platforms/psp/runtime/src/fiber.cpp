#include "fiber.hpp"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <ucontext.h>
#include <vector>
#endif

namespace mhf::psp {

#if defined(_WIN32)

struct Fiber::Impl {
    LPVOID handle{};
    bool owns{};
    Entry entry{};
    void *argument{};
};

namespace {
void WINAPI fiber_start(LPVOID parameter) {
    auto *impl = static_cast<Fiber::Impl *>(parameter);
    impl->entry(impl->argument);
    std::abort(); // a fiber entry must never return
}
} // namespace

std::unique_ptr<Fiber> Fiber::from_current_thread() {
    std::unique_ptr<Fiber> fiber{new Fiber()};
    fiber->impl_ = std::make_unique<Impl>();
    fiber->impl_->handle = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
    if (fiber->impl_->handle == nullptr) {
        throw std::runtime_error("ConvertThreadToFiber failed");
    }
    return fiber;
}

Fiber::Fiber(std::size_t stack_size, Entry entry, void *argument) : impl_(std::make_unique<Impl>()) {
    impl_->entry = entry;
    impl_->argument = argument;
    impl_->owns = true;
    impl_->handle = CreateFiber(stack_size, &fiber_start, impl_.get());
    if (impl_->handle == nullptr) {
        throw std::runtime_error("CreateFiber failed");
    }
}

Fiber::~Fiber() {
    if (impl_ && impl_->owns && impl_->handle != nullptr) {
        DeleteFiber(impl_->handle);
    }
}

void Fiber::switch_to(Fiber & /*from*/, Fiber &to) { SwitchToFiber(to.impl_->handle); }

#else

struct Fiber::Impl {
    ucontext_t context{};
    std::vector<unsigned char> stack;
    Entry entry{};
    void *argument{};
};

namespace {
// makecontext only passes int arguments; split the pointer in two halves.
void fiber_start(unsigned int high, unsigned int low) {
    const auto bits = (static_cast<std::uintptr_t>(high) << 32u) | static_cast<std::uintptr_t>(low);
    auto *impl = reinterpret_cast<Fiber::Impl *>(bits);
    impl->entry(impl->argument);
    std::abort(); // a fiber entry must never return
}
} // namespace

std::unique_ptr<Fiber> Fiber::from_current_thread() {
    std::unique_ptr<Fiber> fiber{new Fiber()};
    fiber->impl_ = std::make_unique<Impl>();
    return fiber; // its context is filled by the first switch away from it
}

Fiber::Fiber(std::size_t stack_size, Entry entry, void *argument) : impl_(std::make_unique<Impl>()) {
    impl_->entry = entry;
    impl_->argument = argument;
    impl_->stack.resize(stack_size);
    if (getcontext(&impl_->context) != 0) {
        throw std::runtime_error("getcontext failed");
    }
    impl_->context.uc_stack.ss_sp = impl_->stack.data();
    impl_->context.uc_stack.ss_size = impl_->stack.size();
    impl_->context.uc_link = nullptr;
    const auto bits = reinterpret_cast<std::uintptr_t>(impl_.get());
    makecontext(&impl_->context, reinterpret_cast<void (*)()>(&fiber_start), 2,
                static_cast<unsigned int>(bits >> 32u), static_cast<unsigned int>(bits & 0xFFFFFFFFu));
}

Fiber::~Fiber() = default;

void Fiber::switch_to(Fiber &from, Fiber &to) {
    if (swapcontext(&from.impl_->context, &to.impl_->context) != 0) {
        std::abort();
    }
}

#endif

} // namespace mhf::psp
