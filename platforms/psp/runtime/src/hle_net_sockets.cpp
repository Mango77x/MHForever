// sceNetAdhoc sockets: PDP datagrams and PTP streams over the relay of the ad hoc server (adhoc_sockets.hpp). A call that
// the PSP would block in waits here the same way: the calling guest thread sleeps in short steps (the others keep running)
// until the socket can do it or the call's timeout passes.

#include "hle_net.hpp"

#include <algorithm>
#include <vector>

namespace mhf::psp::hle {

namespace {

namespace ad = mhf::psp::adhoc;
namespace se = mhf::psp::adhoc::sockerr;

constexpr std::uint32_t kErrorNotInitialized = 0x80410712u;
constexpr std::uint32_t kErrorInvalidDataLen = 0x80410705u;
constexpr std::uint32_t kErrorOffline = 0xFFFFFFFFu; // no server to reach, like a PSP with its wireless switch off
constexpr std::uint32_t kPdpMax = 10 * 1024;         // what the relay carries in one datagram
constexpr std::uint32_t kPtpStatSize = 36;           // next, id, local mac[6], peer mac[6], local port, peer port, send, recv, state
constexpr std::uint64_t kPollStepUs = 1000;

bool is_broadcast(const ad::Mac &mac) {
    return std::all_of(mac.begin(), mac.end(), [](std::uint8_t b) { return b == 0xFF; });
}

ad::Mac read_mac(Runtime &rt, std::uint32_t address) {
    ad::Mac mac{};
    for (std::uint32_t i = 0; i < mac.size(); ++i) mac[i] = rt.memory().load8(address + i);
    return mac;
}

void write_mac(Runtime &rt, std::uint32_t address, const ad::Mac &mac) {
    for (std::uint32_t i = 0; i < mac.size(); ++i) rt.memory().store8(address + i, mac[i]);
}

// A call that fails before it reaches the sockets: not initialized, or no server.
bool unavailable(NetState &s, std::uint32_t &error) {
    if (!s.adhoc_inited) {
        error = kErrorNotInitialized;
        return true;
    }
    if (!s.sockets) {
        error = kErrorOffline;
        return true;
    }
    return false;
}

// Tries `attempt` (which returns the call's result, kWouldBlock when it cannot yet) until it answers: at once for a
// non-blocking call, after `timeout_us` (0: never) with a timeout error for a blocking one.
template <typename Attempt>
std::uint32_t wait_for(Runtime &rt, std::uint32_t timeout_us, bool nonblocking, Attempt &&attempt) {
    const std::uint64_t start = rt.kernel().now_us();
    for (;;) {
        const std::uint32_t result = attempt();
        if (result != se::kWouldBlock || nonblocking) return result;
        const std::uint64_t now = rt.kernel().now_us();
        if (timeout_us != 0 && now - start >= timeout_us) return se::kTimeout;
        rt.kernel().wait(Kernel::WaitType::Delay, 0, now + kPollStepUs);
        if (rt.kernel().exiting()) return se::kTimeout;
    }
}

// ---------------------------------------------------------------- PDP

// sceNetAdhocPdpCreate(mac, port, bufferSize, flag): the socket id. The MAC is filled in with this console's.
void PdpCreate(Runtime &rt, Context &ctx) {
    NetState &s = net_state(rt);
    NET_TRACE("sceNetAdhocPdpCreate(port %u, buffer %d)", arg(ctx, 1) & 0xFFFFu, sarg(ctx, 2));
    std::uint32_t error = 0;
    if (unavailable(s, error)) return ret(ctx, error);
    if (arg(ctx, 0) == 0 || sarg(ctx, 2) <= 0) return ret(ctx, se::kInvalidArg);
    write_mac(rt, arg(ctx, 0), s.mac);
    ret(ctx, static_cast<std::uint32_t>(s.sockets->pdp_create(static_cast<std::uint16_t>(arg(ctx, 1)))));
}

// sceNetAdhocPdpDelete(id, unknown)
void PdpDelete(Runtime &rt, Context &ctx) {
    NetState &s = net_state(rt);
    NET_TRACE("sceNetAdhocPdpDelete(%d)", sarg(ctx, 0));
    std::uint32_t error = 0;
    if (unavailable(s, error)) return ret(ctx, error);
    ret(ctx, s.sockets->pdp_delete(sarg(ctx, 0)));
}

// sceNetAdhocPdpSend(id, destination mac, port, data, length, timeout, flag): to everyone of the group when the MAC is the
// broadcast one.
void PdpSend(Runtime &rt, Context &ctx) {
    NetState &s = net_state(rt);
    const int id = sarg(ctx, 0);
    const std::uint32_t mac_address = arg(ctx, 1), data_address = arg(ctx, 3);
    const std::uint16_t port = static_cast<std::uint16_t>(arg(ctx, 2));
    const std::int32_t length = sarg(ctx, 4);
    const std::uint32_t timeout = arg(ctx, 5);
    const bool nonblocking = arg(ctx, 6) != 0;
    std::uint32_t error = 0;
    if (unavailable(s, error)) return ret(ctx, error);
    if (arg(ctx, 2) == 0 || port == 0) return ret(ctx, se::kInvalidPort);
    if (length < 0 || static_cast<std::uint32_t>(length) > kPdpMax) return ret(ctx, kErrorInvalidDataLen);
    if (data_address == 0) return ret(ctx, se::kInvalidArg);
    if (mac_address == 0) return ret(ctx, se::kInvalidAddr);
    const ad::Mac destination = read_mac(rt, mac_address);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    if (!data.empty()) rt.memory().copy_out(data_address, data);

    std::vector<ad::Mac> targets;
    if (is_broadcast(destination)) {
        for (const ad::Peer &peer : s.adhocctl->peers()) targets.push_back(peer.mac);
    } else {
        targets.push_back(destination);
    }
    NET_TRACE("sceNetAdhocPdpSend(%d, %zu target(s), port %u, %d bytes)", id, targets.size(), port, length);
    for (const ad::Mac &target : targets) {
        const std::uint32_t result = wait_for(rt, timeout, nonblocking, [&] {
            return s.sockets->pdp_send(id, target, port, data.data(), static_cast<std::uint32_t>(data.size()));
        });
        if (result != 0) return ret(ctx, result);
    }
    ret(ctx, 0);
}

// sceNetAdhocPdpRecv(id, source mac, source port (16 bits), buffer, &length, timeout, flag)
void PdpRecv(Runtime &rt, Context &ctx) {
    NetState &s = net_state(rt);
    const int id = sarg(ctx, 0);
    const std::uint32_t mac_address = arg(ctx, 1), port_address = arg(ctx, 2), buffer_address = arg(ctx, 3), length_address = arg(ctx, 4);
    const std::uint32_t timeout = arg(ctx, 5);
    const bool nonblocking = arg(ctx, 6) != 0;
    std::uint32_t error = 0;
    if (unavailable(s, error)) return ret(ctx, error);
    if (mac_address == 0 || port_address == 0 || buffer_address == 0 || length_address == 0) return ret(ctx, se::kInvalidArg);
    const std::int32_t room = static_cast<std::int32_t>(rt.memory().load32(length_address));
    if (room < 0) return ret(ctx, se::kInvalidArg);
    std::vector<std::uint8_t> buffer(std::min<std::size_t>(static_cast<std::size_t>(room), kPdpMax));
    std::uint32_t length = static_cast<std::uint32_t>(buffer.size());
    ad::Mac source{};
    std::uint16_t port = 0;
    const std::uint32_t result = wait_for(rt, timeout, nonblocking, [&] {
        length = static_cast<std::uint32_t>(buffer.size());
        return s.sockets->pdp_recv(id, source, port, buffer.data(), length);
    });
    if (result == 0) {
        write_mac(rt, mac_address, source);
        rt.memory().store16(port_address, port);
        if (length != 0) rt.memory().copy_in(buffer_address, std::span<const std::uint8_t>(buffer.data(), length));
        rt.memory().store32(length_address, length);
        NET_TRACE("sceNetAdhocPdpRecv(%d): %u bytes from port %u", id, length, port);
    } else if (result == se::kNotEnoughSpace) {
        rt.memory().store32(length_address, length); // what the datagram needs
    }
    ret(ctx, result);
}

// ---------------------------------------------------------------- PTP

// sceNetAdhocPtpOpen(source mac, source port, destination mac, destination port, buffer, retransmit interval, count, flag)
void PtpOpen(Runtime &rt, Context &ctx) {
    NetState &s = net_state(rt);
    std::uint32_t error = 0;
    if (unavailable(s, error)) return ret(ctx, error);
    const std::uint32_t source = arg(ctx, 0), destination = arg(ctx, 2);
    if (source == 0 || destination == 0) return ret(ctx, se::kInvalidAddr);
    const ad::Mac peer = read_mac(rt, destination);
    if (is_broadcast(peer) || std::all_of(peer.begin(), peer.end(), [](std::uint8_t b) { return b == 0; })) return ret(ctx, se::kInvalidAddr);
    if (sarg(ctx, 5) <= 0 || sarg(ctx, 6) <= 0 || sarg(ctx, 7) <= 0) return ret(ctx, se::kInvalidArg);
    write_mac(rt, source, s.mac);
    NET_TRACE("sceNetAdhocPtpOpen(port %u -> port %u)", arg(ctx, 1) & 0xFFFFu, arg(ctx, 3) & 0xFFFFu);
    ret(ctx, static_cast<std::uint32_t>(s.sockets->ptp_open(static_cast<std::uint16_t>(arg(ctx, 1)), peer, static_cast<std::uint16_t>(arg(ctx, 3)))));
}

// sceNetAdhocPtpListen(source mac, source port, buffer, retransmit interval, count, backlog, flag)
void PtpListen(Runtime &rt, Context &ctx) {
    NetState &s = net_state(rt);
    std::uint32_t error = 0;
    if (unavailable(s, error)) return ret(ctx, error);
    if (arg(ctx, 0) == 0) return ret(ctx, se::kInvalidAddr);
    if (sarg(ctx, 2) <= 0 || sarg(ctx, 3) <= 0 || sarg(ctx, 4) <= 0 || sarg(ctx, 5) <= 0) return ret(ctx, se::kInvalidArg);
    write_mac(rt, arg(ctx, 0), s.mac);
    NET_TRACE("sceNetAdhocPtpListen(port %u)", arg(ctx, 1) & 0xFFFFu);
    ret(ctx, static_cast<std::uint32_t>(s.sockets->ptp_listen(static_cast<std::uint16_t>(arg(ctx, 1)))));
}

// sceNetAdhocPtpConnect(id, timeout, flag)
void PtpConnect(Runtime &rt, Context &ctx) {
    NetState &s = net_state(rt);
    const int id = sarg(ctx, 0);
    std::uint32_t error = 0;
    if (unavailable(s, error)) return ret(ctx, error);
    const std::uint32_t result = wait_for(rt, arg(ctx, 1), arg(ctx, 2) != 0, [&] { return s.sockets->ptp_connect(id); });
    NET_TRACE("sceNetAdhocPtpConnect(%d) = %08X", id, result);
    ret(ctx, result);
}

// sceNetAdhocPtpAccept(id, peer mac, peer port, timeout, flag): the id of the new connection
void PtpAccept(Runtime &rt, Context &ctx) {
    NetState &s = net_state(rt);
    const int id = sarg(ctx, 0);
    std::uint32_t error = 0;
    if (unavailable(s, error)) return ret(ctx, error);
    ad::Mac peer{};
    std::uint16_t port = 0;
    std::int64_t accepted = 0;
    const std::uint32_t result = wait_for(rt, arg(ctx, 3), arg(ctx, 4) != 0, [&] {
        accepted = s.sockets->ptp_accept(id, peer, port);
        return accepted > 0 ? 0u : static_cast<std::uint32_t>(accepted);
    });
    if (result != 0) return ret(ctx, result);
    if (arg(ctx, 1) != 0) write_mac(rt, arg(ctx, 1), peer);
    if (arg(ctx, 2) != 0) rt.memory().store16(arg(ctx, 2), port);
    NET_TRACE("sceNetAdhocPtpAccept(%d) = connection %d from port %u", id, static_cast<int>(accepted), port);
    ret(ctx, static_cast<std::uint32_t>(accepted));
}

// sceNetAdhocPtpSend(id, data, &length, timeout, flag)
void PtpSend(Runtime &rt, Context &ctx) {
    NetState &s = net_state(rt);
    const int id = sarg(ctx, 0);
    const std::uint32_t data_address = arg(ctx, 1), length_address = arg(ctx, 2);
    std::uint32_t error = 0;
    if (unavailable(s, error)) return ret(ctx, error);
    if (data_address == 0 || length_address == 0) return ret(ctx, se::kInvalidArg);
    const std::int32_t wanted = static_cast<std::int32_t>(rt.memory().load32(length_address));
    if (wanted <= 0) return ret(ctx, se::kInvalidArg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(wanted));
    rt.memory().copy_out(data_address, data);
    std::uint32_t length = static_cast<std::uint32_t>(data.size());
    const std::uint32_t result = wait_for(rt, arg(ctx, 3), arg(ctx, 4) != 0, [&] {
        length = static_cast<std::uint32_t>(data.size());
        return s.sockets->ptp_send(id, data.data(), length);
    });
    if (result == 0) rt.memory().store32(length_address, length);
    NET_TRACE("sceNetAdhocPtpSend(%d, %d bytes) = %08X", id, wanted, result);
    ret(ctx, result);
}

// sceNetAdhocPtpRecv(id, buffer, &length, timeout, flag)
void PtpRecv(Runtime &rt, Context &ctx) {
    NetState &s = net_state(rt);
    const int id = sarg(ctx, 0);
    const std::uint32_t buffer_address = arg(ctx, 1), length_address = arg(ctx, 2);
    std::uint32_t error = 0;
    if (unavailable(s, error)) return ret(ctx, error);
    if (buffer_address == 0 || length_address == 0) return ret(ctx, se::kInvalidArg);
    const std::int32_t room = static_cast<std::int32_t>(rt.memory().load32(length_address));
    if (room <= 0) return ret(ctx, se::kInvalidArg);
    std::vector<std::uint8_t> buffer(std::min<std::size_t>(static_cast<std::size_t>(room), 64 * 1024));
    std::uint32_t length = 0;
    const std::uint32_t result = wait_for(rt, arg(ctx, 3), arg(ctx, 4) != 0, [&] {
        length = static_cast<std::uint32_t>(buffer.size());
        return s.sockets->ptp_recv(id, buffer.data(), length);
    });
    if (result == 0) {
        if (length != 0) rt.memory().copy_in(buffer_address, std::span<const std::uint8_t>(buffer.data(), length));
        rt.memory().store32(length_address, length);
        NET_TRACE("sceNetAdhocPtpRecv(%d): %u bytes", id, length);
    }
    ret(ctx, result);
}

// sceNetAdhocPtpFlush(id, timeout, flag)
void PtpFlush(Runtime &rt, Context &ctx) {
    NetState &s = net_state(rt);
    const int id = sarg(ctx, 0);
    std::uint32_t error = 0;
    if (unavailable(s, error)) return ret(ctx, error);
    ret(ctx, wait_for(rt, arg(ctx, 1), arg(ctx, 2) != 0, [&] { return s.sockets->ptp_flush(id); }));
}

// sceNetAdhocPtpClose(id, unknown)
void PtpClose(Runtime &rt, Context &ctx) {
    NetState &s = net_state(rt);
    NET_TRACE("sceNetAdhocPtpClose(%d)", sarg(ctx, 0));
    std::uint32_t error = 0;
    if (unavailable(s, error)) return ret(ctx, error);
    ret(ctx, s.sockets->ptp_close(sarg(ctx, 0)));
}

// sceNetAdhocGetPtpStat(&size, buffer): the PTP sockets, listed the way the control library lists peers.
void GetPtpStat(Runtime &rt, Context &ctx) {
    NetState &s = net_state(rt);
    std::uint32_t error = 0;
    if (unavailable(s, error)) return ret(ctx, error);
    const std::uint32_t size_address = arg(ctx, 0), buffer = arg(ctx, 1);
    if (size_address == 0) return ret(ctx, se::kInvalidArg);
    auto &m = rt.memory();
    const std::vector<ad::PtpStat> stats = s.sockets->ptp_stats();
    if (buffer == 0) {
        m.store32(size_address, static_cast<std::uint32_t>(stats.size()) * kPtpStatSize);
        return ret(ctx, 0);
    }
    const std::uint32_t capacity = m.load32(size_address) / kPtpStatSize;
    const std::uint32_t count = std::min<std::uint32_t>(capacity, static_cast<std::uint32_t>(stats.size()));
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t entry = buffer + i * kPtpStatSize;
        const ad::PtpStat &stat = stats[i];
        for (std::uint32_t b = 0; b < kPtpStatSize; ++b) m.store8(entry + b, 0);
        m.store32(entry, i + 1 < count ? entry + kPtpStatSize : 0u); // next
        m.store32(entry + 4, static_cast<std::uint32_t>(stat.id));
        write_mac(rt, entry + 8, stat.local);
        write_mac(rt, entry + 14, stat.peer);
        m.store16(entry + 20, stat.local_port);
        m.store16(entry + 22, stat.peer_port);
        m.store32(entry + 24, stat.send_pending);
        m.store32(entry + 28, stat.recv_available);
        m.store32(entry + 32, static_cast<std::uint32_t>(stat.state));
    }
    m.store32(size_address, count * kPtpStatSize);
    ret(ctx, 0);
}

const HleFunction kFunctions[] = {
    {"sceNetAdhoc", 0x6F92741Bu, "sceNetAdhocPdpCreate", &PdpCreate},
    {"sceNetAdhoc", 0x7F27BB5Eu, "sceNetAdhocPdpDelete", &PdpDelete},
    {"sceNetAdhoc", 0xABED3790u, "sceNetAdhocPdpSend", &PdpSend},
    {"sceNetAdhoc", 0xDFE53E03u, "sceNetAdhocPdpRecv", &PdpRecv},
    {"sceNetAdhoc", 0x877F6D66u, "sceNetAdhocPtpOpen", &PtpOpen},
    {"sceNetAdhoc", 0xE08BDAC1u, "sceNetAdhocPtpListen", &PtpListen},
    {"sceNetAdhoc", 0xFC6FC07Bu, "sceNetAdhocPtpConnect", &PtpConnect},
    {"sceNetAdhoc", 0x9DF81198u, "sceNetAdhocPtpAccept", &PtpAccept},
    {"sceNetAdhoc", 0x4DA4C788u, "sceNetAdhocPtpSend", &PtpSend},
    {"sceNetAdhoc", 0x8BEA2B3Eu, "sceNetAdhocPtpRecv", &PtpRecv},
    {"sceNetAdhoc", 0x9AC2EEACu, "sceNetAdhocPtpFlush", &PtpFlush},
    {"sceNetAdhoc", 0x157E6225u, "sceNetAdhocPtpClose", &PtpClose},
    {"sceNetAdhoc", 0xB9685118u, "sceNetAdhocGetPtpStat", &GetPtpStat},
};

} // namespace

std::span<const HleFunction> net_socket_functions() { return kFunctions; }

} // namespace mhf::psp::hle
