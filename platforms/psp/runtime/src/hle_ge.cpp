// sceGe_user: display-list submission (docs/05-runtime-hle.md §6).
//
// Like the real GE, display lists run in parallel with the CPU: a worker thread executes them in submission order
// through the software renderer (platforms/psp/gpu), which draws into guest VRAM, while the game prepares the next
// frame. A list that reaches its stall address waits for sceGeListUpdateStallAddr. Signal and finish callbacks raised
// by the worker are delivered by the scheduler in interrupt context. sceGeListSync/sceGeDrawSync wait for the worker.
// MHF_GE_SYNC=1 runs lists on the submitting thread instead (debugging).

#include "hle.hpp"
#include "kernel.hpp"

#include "mhf/psp/host.hpp"

#include "ge.hpp"

#include <chrono>
#include <atomic>
#include <functional>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mhf::psp {

namespace {

constexpr std::uint32_t kEdramAddress = 0x04000000u;
constexpr std::uint32_t kEdramSize = 0x00200000u;

struct GeCallback {
    std::uint32_t signal_function{}, signal_argument{}, finish_function{}, finish_argument{};
};

struct DisplayList {
    int id{};
    std::uint32_t pc{};
    std::uint32_t stall{};
    int callback{-1};
    std::uint32_t argument{};
    bool done{};
};

// A signal or finish raised by a list, waiting to be delivered to the game.
struct GeEvent {
    int callback{};
    bool finish{};
    std::uint32_t value{};
    std::uint32_t pc{};
};

struct PresentRequest {
    std::uint32_t address{}, stride{}, format{};
};

struct GeState {
    Runtime *rt{};
    std::unique_ptr<gpu::Ge> gpu;
    std::function<std::unique_ptr<gpu::Backend>()> backend_factory;
    std::unique_ptr<gpu::Backend> backend; // lives on the worker thread
    std::atomic<bool> backend_active{false};
    std::deque<PresentRequest> presents;   // queued as negative ids in `queue`
    std::map<int, GeCallback> callbacks; // scheduler side only
    int next_callback{0};

    // Shared with the worker (guarded by mutex).
    std::mutex mutex;
    std::condition_variable wake;
    std::map<int, DisplayList> lists;
    std::deque<int> queue; // lists not finished yet, in execution order
    std::vector<GeEvent> events;
    int next_list{1};
    bool stop{};
    std::atomic<int> pending{0}; // lists queued or running
    std::thread worker;
    bool synchronous{};

    ~GeState() { shutdown(); }

    // Stops the worker (which also destroys the GPU backend on its own thread).
    void shutdown() {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }

    gpu::Ge &renderer() {
        if (!gpu) gpu = std::make_unique<gpu::Ge>(rt->memory());
        return *gpu;
    }

    // Runs the list at the head of the queue until it ends or stalls. Called with `lock` held; releases it while
    // drawing. Returns true when the list ended.
    bool step(std::unique_lock<std::mutex> &lock) {
        const int id = queue.front();
        DisplayList list = lists[id];
        lock.unlock();
        const std::uint64_t started = rt->now_us();
        std::vector<GeEvent> raised;
        const auto result = renderer().run(list.pc, list.stall, [&](bool finish, std::uint32_t value) {
            raised.push_back({list.callback, finish, value, list.pc});
        });
        hle::perf().ge_us += rt->now_us() - started;
        lock.lock();
        auto &stored = lists[id];
        stored.pc = result.pc;
        if (!raised.empty()) {
            events.insert(events.end(), raised.begin(), raised.end());
            abi::interrupt_pending.store(true, std::memory_order_relaxed); // deliver them promptly
        }
        if (result.ended) {
            stored.done = true;
            queue.pop_front();
            --pending;
            // Keep the table small: completed lists are only needed for ListSync.
            while (lists.size() > 64 && lists.begin()->second.done) lists.erase(lists.begin());
            return true;
        }
        return false;
    }

    void worker_main() {
        if (backend_factory) {
            backend = backend_factory();
            if (backend) {
                renderer().set_backend(backend.get());
                backend_active = true;
            }
        }
        std::unique_lock lock(mutex);
        // Waits for work, but not past the moment the backend wants to be called (a second picture for a flip).
        const auto wait_for_work = [&] {
            const auto due = backend ? backend->next_wakeup() : std::nullopt;
            if (due) wake.wait_until(lock, *due);
            else wake.wait(lock);
        };
        // The list at the head stalled with nothing to draw: the GE is idle until the game adds to it.
        int stalled_id = -1;
        std::uint32_t stalled_pc = 0, stalled_stall = 0;
        const auto idle = [&] {
            if (queue.empty()) return true;
            const int id = queue.front();
            if (id < 0) return false;
            const DisplayList &l = lists[id];
            return id == stalled_id && l.pc == stalled_pc && l.stall == stalled_stall;
        };
        while (!stop) {
            hle::ge_thread_cpu_us().store(hle::thread_cpu_us(), std::memory_order_relaxed);
            hle::ge_detail().transform_us.store(renderer().stats().transform_ns / 1000u, std::memory_order_relaxed);
            hle::ge_detail().assemble_us.store(renderer().stats().assemble_ns / 1000u, std::memory_order_relaxed);
            if (backend) {
                const auto due = backend->next_wakeup();
                // Pictures the backend adds on its own (frame interpolation) only take time the game's drawing does
                // not need: a busy scene shows fewer of them instead of slowing the game down.
                if (due && std::chrono::steady_clock::now() >= *due && idle()) {
                    lock.unlock();
                    backend->tick();
                    lock.lock();
                    continue;
                }
            }
            if (queue.empty()) {
                wait_for_work();
                continue;
            }
            if (queue.front() < 0) {
                const PresentRequest p = presents.front();
                presents.pop_front();
                queue.pop_front();
                if (backend) {
                    lock.unlock();
                    backend->present(p.address, p.stride, p.format);
                    lock.lock();
                }
                continue;
            }
            const DisplayList &head = lists[queue.front()];
            const std::uint32_t pc = head.pc, stall = head.stall;
            if (!step(lock) && lists[queue.front()].pc == pc && lists[queue.front()].stall == stall) {
                // Stalled with nothing new to draw: wait for sceGeListUpdateStallAddr.
                stalled_id = queue.front();
                stalled_pc = pc;
                stalled_stall = stall;
                wait_for_work();
            }
        }
        lock.unlock();
        if (gpu) gpu->set_backend(nullptr);
        backend.reset(); // its OpenGL context belongs to this thread
        backend_active = false;
    }

    void submit_changed() {
        if (synchronous) {
            std::unique_lock lock(mutex);
            while (!queue.empty()) {
                const std::uint32_t pc = lists[queue.front()].pc;
                if (!step(lock) && lists[queue.front()].pc == pc) break; // stalled
            }
            return;
        }
        if (!worker.joinable()) worker = std::thread([this] { worker_main(); });
        wake.notify_all();
    }

    // Scheduler side: delivers queued signal/finish callbacks.
    void deliver_events() {
        std::vector<GeEvent> ready;
        {
            std::lock_guard lock(mutex);
            if (events.empty()) return;
            ready.swap(events);
        }
        for (const auto &e : ready) {
            const auto it = callbacks.find(e.callback);
            if (it == callbacks.end()) continue;
            const std::uint32_t function = e.finish ? it->second.finish_function : it->second.signal_function;
            const std::uint32_t argument = e.finish ? it->second.finish_argument : it->second.signal_argument;
            if (function != 0) rt->kernel().call_from_interrupt(function, {e.value, argument, e.pc});
        }
    }
};

std::unordered_map<const Runtime *, std::unique_ptr<GeState>> g_ge;

GeState &ge(Runtime &rt) {
    auto &slot = g_ge[&rt];
    if (!slot) {
        slot = std::make_unique<GeState>();
        slot->rt = &rt;
        const char *sync = std::getenv("MHF_GE_SYNC");
        slot->synchronous = sync != nullptr && sync[0] == '1';
        GeState *state = slot.get();
        rt.kernel().pollers.push_back([state] { state->deliver_events(); });
    }
    return *slot;
}

// Blocks the calling guest thread until `done()` holds, polling every 100 us (the worker cannot wake guest threads).
template <typename Done>
void wait_for_ge(Runtime &rt, Done done) {
    while (!done()) {
        rt.kernel().wait(Kernel::WaitType::Delay, 0, rt.kernel().now_us() + 100u);
    }
}

} // namespace

namespace hle {
namespace {

void GeEdramGetAddr(Runtime &, Context &ctx) { ret(ctx, kEdramAddress); }
void GeEdramGetSize(Runtime &, Context &ctx) { ret(ctx, kEdramSize); }
void GeEdramSetAddrTranslation(Runtime &, Context &ctx) { ret(ctx, 0); }

void GeSetCallback(Runtime &rt, Context &ctx) {
    auto &s = ge(rt);
    const std::uint32_t data = arg(ctx, 0);
    GeCallback cb;
    cb.signal_function = rt.memory().load32(data + 0);
    cb.signal_argument = rt.memory().load32(data + 4);
    cb.finish_function = rt.memory().load32(data + 8);
    cb.finish_argument = rt.memory().load32(data + 12);
    const int id = s.next_callback++;
    s.callbacks[id] = cb;
    ret(ctx, static_cast<std::uint32_t>(id));
}

void GeUnsetCallback(Runtime &rt, Context &ctx) {
    ge(rt).callbacks.erase(sarg(ctx, 0));
    ret(ctx, 0);
}

void enqueue(Runtime &rt, Context &ctx, bool head) {
    auto &s = ge(rt);
    int id;
    {
        std::lock_guard lock(s.mutex);
        DisplayList list;
        list.id = id = s.next_list++;
        list.pc = arg(ctx, 0) & 0x0FFFFFFFu;
        list.stall = arg(ctx, 1) & 0x0FFFFFFFu;
        list.callback = sarg(ctx, 2);
        list.argument = arg(ctx, 3);
        s.lists[id] = list;
        // A list added at the head runs next, but never interrupts the one already being drawn.
        if (head && s.queue.size() > 1) s.queue.insert(s.queue.begin() + 1, id);
        else s.queue.push_back(id);
        ++s.pending;
    }
    ret(ctx, static_cast<std::uint32_t>(id));
    s.submit_changed();
}
void GeListEnQueue(Runtime &rt, Context &ctx) { enqueue(rt, ctx, false); }
void GeListEnQueueHead(Runtime &rt, Context &ctx) { enqueue(rt, ctx, true); }

void GeListUpdateStallAddr(Runtime &rt, Context &ctx) {
    auto &s = ge(rt);
    {
        std::lock_guard lock(s.mutex);
        const auto it = s.lists.find(sarg(ctx, 0));
        if (it == s.lists.end()) {
            ret(ctx, 0x80000100u); // SCE_KERNEL_ERROR_INVALID_ID
            return;
        }
        it->second.stall = arg(ctx, 1) & 0x0FFFFFFFu;
    }
    ret(ctx, 0);
    s.submit_changed();
}

// sceGeListSync(id, mode): mode 0 waits for the list, 1 peeks. 0 = completed, 2 = drawing.
void GeListSync(Runtime &rt, Context &ctx) {
    auto &s = ge(rt);
    const int id = sarg(ctx, 0);
    auto finished = [&] {
        std::lock_guard lock(s.mutex);
        const auto it = s.lists.find(id);
        return it == s.lists.end() || it->second.done;
    };
    if (arg(ctx, 1) == 0) {
        wait_for_ge(rt, finished);
        ret(ctx, 0);
        return;
    }
    ret(ctx, finished() ? 0u : 2u);
}

// sceGeDrawSync(mode): mode 0 waits until every list has finished, 1 peeks.
void GeDrawSync(Runtime &rt, Context &ctx) {
    auto &s = ge(rt);
    auto idle = [&] { return s.pending.load() == 0; };
    if (arg(ctx, 0) == 0) {
        wait_for_ge(rt, idle);
        ret(ctx, 0);
        return;
    }
    ret(ctx, idle() ? 0u : 2u);
}

void GeListDeQueue(Runtime &rt, Context &ctx) {
    auto &s = ge(rt);
    std::lock_guard lock(s.mutex);
    const int id = sarg(ctx, 0);
    // Only a list that has not started can be removed.
    for (std::size_t i = 1; i < s.queue.size(); ++i) {
        if (s.queue[i] == id) {
            s.queue.erase(s.queue.begin() + static_cast<std::ptrdiff_t>(i));
            s.lists.erase(id);
            --s.pending;
            break;
        }
    }
    ret(ctx, 0);
}
void GeBreakContinue(Runtime &, Context &ctx) { ret(ctx, 0); }

const HleFunction kFunctions[] = {
    {"sceGe_user", 0xE47E40E4u, "sceGeEdramGetAddr", &GeEdramGetAddr},
    {"sceGe_user", 0x1F6752ADu, "sceGeEdramGetSize", &GeEdramGetSize},
    {"sceGe_user", 0xB77905EAu, "sceGeEdramSetAddrTranslation", &GeEdramSetAddrTranslation},
    {"sceGe_user", 0xA4FC06A4u, "sceGeSetCallback", &GeSetCallback},
    {"sceGe_user", 0x05DB22CEu, "sceGeUnsetCallback", &GeUnsetCallback},
    {"sceGe_user", 0xAB49E76Au, "sceGeListEnQueue", &GeListEnQueue},
    {"sceGe_user", 0x1C0D95A6u, "sceGeListEnQueueHead", &GeListEnQueueHead},
    {"sceGe_user", 0xE0D68148u, "sceGeListUpdateStallAddr", &GeListUpdateStallAddr},
    {"sceGe_user", 0x03444EB4u, "sceGeListSync", &GeListSync},
    {"sceGe_user", 0xB287BD61u, "sceGeDrawSync", &GeDrawSync},
    {"sceGe_user", 0x5FB86AB0u, "sceGeListDeQueue", &GeListDeQueue},
    {"sceGe_user", 0xB448EC0Du, "sceGeBreak", &GeBreakContinue},
    {"sceGe_user", 0x4C06E472u, "sceGeContinue", &GeBreakContinue},
};

} // namespace

std::span<const HleFunction> ge_functions() { return kFunctions; }

void ge_shutdown(Runtime &rt) {
    if (const auto it = g_ge.find(&rt); it != g_ge.end() && it->second) it->second->shutdown();
}

void ge_release(Runtime &rt) { g_ge.erase(&rt); }

void ge_present(Runtime &rt, std::uint32_t address, std::uint32_t stride, std::uint32_t format) {
    auto &s = ge(rt);
    if (!s.backend_factory || s.synchronous) return;
    {
        std::lock_guard lock(s.mutex);
        s.presents.push_back({address, stride, format});
        s.queue.push_back(-1);
    }
    s.submit_changed();
}

} // namespace hle
} // namespace mhf::psp

namespace mhf::psp {

void set_gpu_backend_factory(Runtime &rt, std::function<std::unique_ptr<gpu::Backend>()> factory) {
    ge(rt).backend_factory = std::move(factory);
}

bool gpu_backend_active(Runtime &rt) { return ge(rt).backend_active.load(); }

void stop_gpu_backend(Runtime &rt) { hle::ge_shutdown(rt); }

} // namespace mhf::psp
