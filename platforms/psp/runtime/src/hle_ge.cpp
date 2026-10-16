// sceGe_user: display-list submission (docs/05-runtime-hle.md §6).
//
// Lists run synchronously on submission through the software renderer (platforms/psp/gpu), which draws into guest
// VRAM. Signal and finish callbacks fire in interrupt context while the list runs.

#include "hle.hpp"
#include "kernel.hpp"

#include "mhf/psp/host.hpp"

#include "ge.hpp"

#include <memory>

#include <array>
#include <cstdio>
#include <map>
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

struct GeState {
    std::unique_ptr<gpu::Ge> gpu;
    std::map<int, GeCallback> callbacks;
    std::map<int, DisplayList> lists;
    int next_list{1};
    int next_callback{0};
    std::uint64_t lists_completed{};
};

std::unordered_map<const Runtime *, GeState> g_ge;
GeState &ge(Runtime &rt) { return g_ge[&rt]; }

void fire(Runtime &rt, const DisplayList &list, bool finish, std::uint32_t value) {
    auto &s = ge(rt);
    const auto it = s.callbacks.find(list.callback);
    if (it == s.callbacks.end()) {
        return;
    }
    const std::uint32_t function = finish ? it->second.finish_function : it->second.signal_function;
    const std::uint32_t argument = finish ? it->second.finish_argument : it->second.signal_argument;
    if (function != 0) {
        rt.kernel().call_from_interrupt(function, {value, argument, list.pc});
    }
}

// Runs a list until it ends or reaches its stall address.
void run_list(Runtime &rt, DisplayList &list) {
    auto &s = ge(rt);
    if (!s.gpu) {
        s.gpu = std::make_unique<gpu::Ge>(rt.memory());
    }
    const auto result = s.gpu->run(list.pc, list.stall, [&](bool finish, std::uint32_t value) {
        fire(rt, list, finish, value);
    });
    list.pc = result.pc;
    if (result.ended) {
        list.done = true;
        ++s.lists_completed;
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

void enqueue(Runtime &rt, Context &ctx) {
    auto &s = ge(rt);
    DisplayList list;
    list.id = s.next_list++;
    list.pc = arg(ctx, 0) & 0x0FFFFFFFu;
    list.stall = arg(ctx, 1) & 0x0FFFFFFFu;
    list.callback = sarg(ctx, 2);
    list.argument = arg(ctx, 3);
    const int id = list.id;
    auto &stored = s.lists[id] = std::move(list);
    ret(ctx, static_cast<std::uint32_t>(id));
    run_list(rt, stored);
    // Keep the table small: completed lists are only needed for ListSync.
    while (s.lists.size() > 64 && s.lists.begin()->second.done) {
        s.lists.erase(s.lists.begin());
    }
}

void GeListUpdateStallAddr(Runtime &rt, Context &ctx) {
    auto &s = ge(rt);
    const auto it = s.lists.find(sarg(ctx, 0));
    if (it == s.lists.end()) {
        ret(ctx, 0x80000100u); // SCE_KERNEL_ERROR_INVALID_ID
        return;
    }
    it->second.stall = arg(ctx, 1) & 0x0FFFFFFFu;
    ret(ctx, 0);
    if (!it->second.done) run_list(rt, it->second);
}

void GeListSync(Runtime &rt, Context &ctx) {
    const auto &s = ge(rt);
    const auto it = s.lists.find(sarg(ctx, 0));
    // 0 = completed, 2 = drawing (stalled lists count as drawing).
    ret(ctx, it == s.lists.end() || it->second.done ? 0u : 2u);
}

void GeDrawSync(Runtime &, Context &ctx) { ret(ctx, 0); }
void GeListDeQueue(Runtime &rt, Context &ctx) {
    ge(rt).lists.erase(sarg(ctx, 0));
    ret(ctx, 0);
}
void GeBreakContinue(Runtime &, Context &ctx) { ret(ctx, 0); }

const HleFunction kFunctions[] = {
    {"sceGe_user", 0xE47E40E4u, "sceGeEdramGetAddr", &GeEdramGetAddr},
    {"sceGe_user", 0x1F6752ADu, "sceGeEdramGetSize", &GeEdramGetSize},
    {"sceGe_user", 0xB77905EAu, "sceGeEdramSetAddrTranslation", &GeEdramSetAddrTranslation},
    {"sceGe_user", 0xA4FC06A4u, "sceGeSetCallback", &GeSetCallback},
    {"sceGe_user", 0x05DB22CEu, "sceGeUnsetCallback", &GeUnsetCallback},
    {"sceGe_user", 0xAB49E76Au, "sceGeListEnQueue", &enqueue},
    {"sceGe_user", 0x1C0D95A6u, "sceGeListEnQueueHead", &enqueue},
    {"sceGe_user", 0xE0D68148u, "sceGeListUpdateStallAddr", &GeListUpdateStallAddr},
    {"sceGe_user", 0x03444EB4u, "sceGeListSync", &GeListSync},
    {"sceGe_user", 0xB287BD61u, "sceGeDrawSync", &GeDrawSync},
    {"sceGe_user", 0x5FB86AB0u, "sceGeListDeQueue", &GeListDeQueue},
    {"sceGe_user", 0xB448EC0Du, "sceGeBreak", &GeBreakContinue},
    {"sceGe_user", 0x4C06E472u, "sceGeContinue", &GeBreakContinue},
};

} // namespace

std::span<const HleFunction> ge_functions() { return kFunctions; }

} // namespace hle
} // namespace mhf::psp
