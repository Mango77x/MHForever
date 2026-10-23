#pragma once

// The state of the ad hoc networking of one Runtime, shared by the HLE of the control library (hle_net.cpp) and of the sockets
// (hle_net_sockets.cpp). docs/08-multiplayer.md section 1.2.

#include "hle.hpp"
#include "kernel.hpp"

#include "adhoc_link_aemu.hpp"
#include "adhoc_sockets.hpp"
#include "adhocctl.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace mhf::psp::hle {

// MHF_TRACE_ADHOC=1 logs the ad hoc calls the game makes and what the library tells its handlers.
[[nodiscard]] bool net_tracing();
#define NET_TRACE(...)                                    \
    do {                                                  \
        if (::mhf::psp::hle::net_tracing()) {             \
            std::fprintf(stderr, "[adhoc] " __VA_ARGS__); \
            std::fputc('\n', stderr);                     \
        }                                                 \
    } while (0)

constexpr std::int32_t kDiscoverNone = 0, kDiscoverInProgress = 1, kDiscoverCompleted = 2;

struct NetState {
    Runtime *rt{};
    std::unique_ptr<adhoc::AemuLink> link; // null: no server configured
    std::unique_ptr<adhoc::Adhocctl> adhocctl;
    std::unique_ptr<adhoc::Sockets> sockets; // between sceNetAdhocInit and sceNetAdhocTerm, with a server
    bool adhoc_inited = false;               // sceNetAdhocInit
    bool poller_added = false;
    adhoc::Mac mac{};
    std::int32_t discover_status = kDiscoverNone;
    std::uint32_t discover_params = 0;
    bool discover_stopping = false;

    // Called by the kernel on every scheduler pass: tells the game's handlers what the library saw.
    void deliver() {
        if (!adhocctl || !adhocctl->initialized()) return;
        for (const adhoc::Notification &n : adhocctl->poll()) {
            NET_TRACE("event %d error %d to %zu handler(s)", n.event, n.error, adhocctl->handlers().size());
            const std::vector<adhoc::Handler> handlers = adhocctl->handlers(); // a handler may add or remove handlers
            for (const adhoc::Handler &h : handlers) {
                if (h.function != 0) {
                    rt->kernel().call_from_interrupt(h.function, {static_cast<std::uint32_t>(n.event),
                                                                  static_cast<std::uint32_t>(n.error), h.argument});
                }
            }
        }
    }
};

[[nodiscard]] NetState &net_state(Runtime &rt);

} // namespace mhf::psp::hle
