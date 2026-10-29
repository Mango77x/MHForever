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
// MHF_TRACE_ADHOC=2 also dumps the start of every datagram (for working out what a game sends).
[[nodiscard]] bool net_tracing_packets();
void net_trace_packet(const char *direction, double seconds, int socket, const std::uint8_t *data, std::uint32_t length);
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
    std::string room; // the private room (Runtime::set_adhoc_room), kept here until the library exists
    adhoc::Mac mac{};
    std::int32_t discover_status = kDiscoverNone;
    std::uint32_t discover_params = 0;
    bool discover_stopping = false;

    // MHF_TRACE_ADHOC=1: what the game does with its datagrams, summed up once a second.
    struct Traffic {
        std::uint64_t window_start_us = 0;
        int sent = 0, send_blocked = 0, received = 0, receive_empty = 0;
        std::uint64_t sent_bytes = 0, received_bytes = 0;
    } traffic;
    // Counts one datagram call and prints the summary when a second has passed.
    void note_datagram(char kind, std::uint32_t result, std::uint32_t bytes) {
        if (!net_tracing()) return;
        if (kind == 's') {
            if (result == 0) { ++traffic.sent; traffic.sent_bytes += bytes; }
            else ++traffic.send_blocked;
        } else {
            if (result == 0) { ++traffic.received; traffic.received_bytes += bytes; }
            else ++traffic.receive_empty;
        }
        const std::uint64_t now = rt->now_us();
        if (traffic.window_start_us == 0) traffic.window_start_us = now;
        if (now - traffic.window_start_us < 1'000'000) return;
        std::fprintf(stderr, "[adhoc] %.1f s: sent %d (%llu bytes, %d refused), received %d (%llu bytes), %d empty reads\n",
                     static_cast<double>(now) / 1e6, traffic.sent, static_cast<unsigned long long>(traffic.sent_bytes), traffic.send_blocked,
                     traffic.received, static_cast<unsigned long long>(traffic.received_bytes), traffic.receive_empty);
        traffic = {};
        traffic.window_start_us = now;
    }

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
