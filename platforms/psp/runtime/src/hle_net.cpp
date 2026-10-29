// sceNet, sceWlan, sceNetApctl, sceNetInet (stubs: the game never reaches the internet), sceNetAdhoc (init, term),
// sceNetAdhocctl and sceNetAdhocDiscover: the PSP's ad hoc networking on top of an ad hoc server (docs/08-multiplayer.md
// section 1.2). The socket calls (PDP and PTP) are in hle_net_sockets.cpp.

#include "hle_net.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <unordered_map>

namespace mhf::psp::hle {

namespace {

namespace ad = mhf::psp::adhoc;

constexpr std::uint32_t kErrorAlreadyInitialized = 0x80410713u;
constexpr std::uint32_t kErrorInvalidArg = 0x80410B04u;
constexpr std::uint32_t kErrorAdhocctlNotInitialized = 0x80410B08u;

// Sizes of the structures the game reads back (packed, as on the PSP).
constexpr std::uint32_t kPeerInfoSize = 152; // next, nickname[128], mac[6], padding[2], flags, last_recv (u64)
constexpr std::uint32_t kScanInfoSize = 28;  // next, channel, group name[8], bssid (mac[6] + padding[2]), mode

constexpr std::uint32_t kDiscoverResultOffset = 12; // SceNetAdhocDiscoverParam {unknown1, groupName[8], unknown2, result}
constexpr std::int32_t kDiscoverNoPeer = 0, kDiscoverCanceled = 1;

std::unordered_map<const Runtime *, std::unique_ptr<NetState>> g_net;

// The MAC address of this console on the virtual network: the one the launcher gave, or one made from the nickname (a
// locally administered address, the same every time).
ad::Mac make_mac(const RuntimeOptions &o) {
    ad::Mac mac{};
    std::memcpy(mac.data(), o.adhoc_mac.data(), mac.size());
    if (std::any_of(mac.begin(), mac.end(), [](std::uint8_t b) { return b != 0; })) return mac;
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const char c : o.adhoc_nickname + o.adhoc_product_code) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x100000001B3ull;
    }
    mac[0] = 0x02;
    for (std::size_t i = 1; i < mac.size(); ++i) mac[i] = static_cast<std::uint8_t>(h >> (8u * (i - 1u)));
    return mac;
}

NetState &net(Runtime &rt) {
    auto &slot = g_net[&rt];
    if (!slot) {
        slot = std::make_unique<NetState>();
        slot->rt = &rt;
        slot->mac = make_mac(rt.options());
        slot->room = ad::normalize_room_code(rt.options().adhoc_room);
        slot->server = rt.options().adhoc_server;
        slot->adhocctl = std::make_unique<ad::Adhocctl>(nullptr);
    }
    return *slot;
}

std::unique_ptr<ad::AemuLink> make_link(const NetState &s, const std::string &server) {
    if (server.empty()) return nullptr;
    const RuntimeOptions &o = s.rt->options();
    ad::AemuLink::Config config;
    config.host = server;
    config.product_code = o.adhoc_product_code;
    config.nickname = o.adhoc_nickname;
    config.mac = s.mac;
    return std::make_unique<ad::AemuLink>(std::move(config));
}

// The sockets go through the relay of the server (the next port up, as with PPSSPP's servers); null without one.
std::unique_ptr<ad::Sockets> make_sockets(const NetState &s) {
    std::string host;
    std::uint16_t port = 0;
    if (!ad::AemuLink::split_address(s.server, host, port)) return nullptr;
    ad::Sockets::Config config;
    config.host = host;
    config.relay_port = static_cast<std::uint16_t>(port + 1);
    config.mac = s.mac;
    return std::make_unique<ad::Sockets>(std::move(config));
}

// Moves to the server Runtime::set_adhoc_server chose once that cannot cut anything off: in no hall, joining or
// scanning none, no socket open (the village; a hall or a quest has to be left first).
void apply_server(NetState &s) {
    if (!s.server_pending) return;
    if (!s.poller_added) { // nothing contacted yet: start_link uses it
        s.server = s.next_server;
        s.server_pending = false;
        return;
    }
    if (!s.adhocctl->idle() || (s.sockets && !s.sockets->empty())) return;
    std::unique_ptr<ad::AemuLink> link = make_link(s, s.next_server);
    if (!s.adhocctl->set_link(link.get())) return;
    s.link = std::move(link); // the old one logs out
    s.server = s.next_server;
    s.server_pending = false;
    if (s.adhoc_inited) s.sockets = make_sockets(s);
    NET_TRACE("server changed: %s", s.server.c_str());
}

// Starts talking to the ad hoc server the first time the game uses the ad hoc libraries: nothing is contacted before.
void start_link(NetState &s) {
    if (s.poller_added) return;
    apply_server(s);
    s.poller_added = true;
    const RuntimeOptions &o = s.rt->options();
    s.link = make_link(s, s.server);
    const bool was_initialized = s.adhocctl->initialized();
    s.adhocctl = std::make_unique<ad::Adhocctl>(s.link.get());
    if (was_initialized) (void)s.adhocctl->init();
    s.adhocctl->set_room(s.room);
    for (const std::string &text : o.adhoc_hall_prefixes) {
        ad::HallPrefix prefix{};
        std::copy_n(text.begin(), std::min(text.size(), prefix.size()), prefix.begin());
        s.adhocctl->add_hall_prefix(prefix);
    }
    if (!o.adhoc_room.empty()) {
        NET_TRACE("private room %s", s.room.empty() ? "(not a valid code: public halls)" : s.room.c_str());
    }
    NetState *state = &s;
    s.rt->kernel().pollers.push_back([state] {
        apply_server(*state);
        state->deliver();
    });
}

void write_mac(Runtime &rt, std::uint32_t address, const ad::Mac &mac) {
    for (std::uint32_t i = 0; i < mac.size(); ++i) rt.memory().store8(address + i, mac[i]);
}

// ---------------------------------------------------------------- sceNet

void NetInit(Runtime &, Context &ctx) { ret(ctx, 0); }
void NetTerm(Runtime &, Context &ctx) { ret(ctx, 0); }
void NetFreeThreadinfo(Runtime &, Context &ctx) { ret(ctx, 0); }
void NetGetLocalEtherAddr(Runtime &rt, Context &ctx) {
    if (arg(ctx, 0) != 0) write_mac(rt, arg(ctx, 0), net(rt).mac);
    ret(ctx, 0);
}

// ---------------------------------------------------------------- sceWlanDrv

void WlanGetEtherAddr(Runtime &rt, Context &ctx) {
    if (arg(ctx, 0) != 0) write_mac(rt, arg(ctx, 0), net(rt).mac);
    ret(ctx, 0);
}
void WlanGetSwitchState(Runtime &, Context &ctx) { ret(ctx, 1); } // the wireless switch is on

// ---------------------------------------------------------------- sceNetApctl, sceNetInet: no internet

void ApctlOk(Runtime &, Context &ctx) { ret(ctx, 0); }
void ApctlGetInfo(Runtime &, Context &ctx) { ret(ctx, 0x80410B03u); } // as with the wireless switch off: nothing to report
void InetFail(Runtime &, Context &ctx) { ret(ctx, 0xFFFFFFFFu); }

// ---------------------------------------------------------------- sceNetAdhoc (sockets are in hle_net_sockets.cpp)

void AdhocInit(Runtime &rt, Context &ctx) {
    NetState &s = net(rt);
    NET_TRACE("sceNetAdhocInit");
    if (s.adhoc_inited) return ret(ctx, kErrorAlreadyInitialized);
    s.adhoc_inited = true;
    start_link(s);
    s.sockets = make_sockets(s);
    ret(ctx, 0);
}
void AdhocTerm(Runtime &rt, Context &ctx) {
    NET_TRACE("sceNetAdhocTerm");
    NetState &s = net(rt);
    s.adhoc_inited = false;
    s.sockets.reset(); // every socket closes
    ret(ctx, 0);
}

// ---------------------------------------------------------------- sceNetAdhocctl

void AdhocctlInit(Runtime &rt, Context &ctx) {
    NetState &s = net(rt);
    NET_TRACE("sceNetAdhocctlInit");
    start_link(s);
    ret(ctx, s.adhocctl->init());
}
void AdhocctlTerm(Runtime &rt, Context &ctx) {
    NET_TRACE("sceNetAdhocctlTerm");
    ret(ctx, net(rt).adhocctl->term());
}

void AdhocctlAddHandler(Runtime &rt, Context &ctx) {
    NET_TRACE("sceNetAdhocctlAddHandler(%08X, %08X)", arg(ctx, 0), arg(ctx, 1));
    ret(ctx, static_cast<std::uint32_t>(net(rt).adhocctl->add_handler(arg(ctx, 0), arg(ctx, 1))));
}
void AdhocctlDelHandler(Runtime &rt, Context &ctx) { ret(ctx, net(rt).adhocctl->del_handler(sarg(ctx, 0))); }
void AdhocctlDisconnect(Runtime &rt, Context &ctx) {
    NET_TRACE("sceNetAdhocctlDisconnect");
    ret(ctx, net(rt).adhocctl->disconnect());
}
void AdhocctlScan(Runtime &rt, Context &ctx) {
    NET_TRACE("sceNetAdhocctlScan");
    ret(ctx, net(rt).adhocctl->scan());
}

// GetScanInfo(&size, buffer): without a buffer the size needed is reported; with one, as many entries as fit are filled
// in, chained through their `next` pointers, and the size is what was written. Inside a group there is nothing to scan.
void AdhocctlGetScanInfo(Runtime &rt, Context &ctx) {
    ad::Adhocctl &c = *net(rt).adhocctl;
    if (!c.initialized()) return ret(ctx, kErrorAdhocctlNotInitialized);
    const std::uint32_t size_address = arg(ctx, 0), buffer = arg(ctx, 1);
    NET_TRACE("sceNetAdhocctlGetScanInfo(%08X, %08X): %zu group(s)", size_address, buffer, c.groups().size());
    if (size_address == 0) return ret(ctx, kErrorInvalidArg);
    auto &m = rt.memory();
    const auto &groups = c.groups();
    const std::uint32_t available = c.state() == ad::State::Connected ? 0u : static_cast<std::uint32_t>(groups.size());
    if (buffer == 0) {
        m.store32(size_address, available * kScanInfoSize);
        return ret(ctx, 0);
    }
    const std::uint32_t capacity = m.load32(size_address) / kScanInfoSize;
    const std::uint32_t count = std::min(capacity, available);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t entry = buffer + i * kScanInfoSize;
        for (std::uint32_t b = 0; b < kScanInfoSize; ++b) m.store8(entry + b, 0);
        m.store32(entry, i + 1 < count ? entry + kScanInfoSize : 0u); // next
        m.store32(entry + 4, 1);                                       // channel
        for (std::uint32_t b = 0; b < 8; ++b) m.store8(entry + 8 + b, static_cast<std::uint8_t>(groups[i].name[b]));
        write_mac(rt, entry + 16, groups[i].leader); // bssid
    }
    m.store32(size_address, count * kScanInfoSize);
    if (net_tracing() && count > 0) {
        std::string names;
        for (std::uint32_t i = 0; i < count; ++i) names += (i == 0 ? "" : " ") + ad::printable(groups[i].name);
        NET_TRACE("  groups: %s", names.c_str());
    }
    ret(ctx, 0);
}

// GetPeerList(&size, buffer): the other members of the group, the same way.
void AdhocctlGetPeerList(Runtime &rt, Context &ctx) {
    ad::Adhocctl &c = *net(rt).adhocctl;
    if (!c.initialized()) return ret(ctx, kErrorAdhocctlNotInitialized);
    const std::uint32_t size_address = arg(ctx, 0), buffer = arg(ctx, 1);
    NET_TRACE("sceNetAdhocctlGetPeerList(%08X, %08X): %zu peer(s)", size_address, buffer, c.peers().size());
    if (size_address == 0) return ret(ctx, kErrorInvalidArg);
    auto &m = rt.memory();
    const auto &peers = c.peers();
    const std::uint32_t available = static_cast<std::uint32_t>(peers.size());
    if (buffer == 0) {
        m.store32(size_address, available * kPeerInfoSize);
        return ret(ctx, 0);
    }
    const std::uint32_t capacity = m.load32(size_address) / kPeerInfoSize;
    const std::uint32_t count = std::min(capacity, available);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t entry = buffer + i * kPeerInfoSize;
        for (std::uint32_t b = 0; b < kPeerInfoSize; ++b) m.store8(entry + b, 0);
        m.store32(entry, i + 1 < count ? entry + kPeerInfoSize : 0u); // next
        const std::string &name = peers[i].nickname;
        for (std::uint32_t b = 0; b < 127 && b < name.size(); ++b) m.store8(entry + 4 + b, static_cast<std::uint8_t>(name[b]));
        write_mac(rt, entry + 132, peers[i].mac);
        m.store32(entry + 140, 0x0400u); // flags, as PPSSPP reports them
        // When the peer was last heard from, on the clock the game reads with sceKernelGetSystemTimeWide: games judge a peer's
        // connection by it. PPSSPP reports "10 ms ago" for every peer of the group (one that never says anything would
        // be flagged as lost), and so do we while the server keeps the group together.
        const std::uint64_t now = rt.now_us();
        const std::uint64_t heard = now > 10'000u ? now - 10'000u : 0u;
        m.store32(entry + 144, static_cast<std::uint32_t>(heard));
        m.store32(entry + 148, static_cast<std::uint32_t>(heard >> 32u));
    }
    m.store32(size_address, count * kPeerInfoSize);
    ret(ctx, 0);
}

// ---------------------------------------------------------------- sceNetAdhocDiscover
// A stub that finds nobody, with the states a game checks (PPSSPP does the same).

void DiscoverInitStart(Runtime &rt, Context &ctx) {
    NetState &s = net(rt);
    NET_TRACE("sceNetAdhocDiscoverInitStart(%08X)", arg(ctx, 0));
    if (arg(ctx, 0) == 0) return ret(ctx, kErrorInvalidArg);
    s.discover_params = arg(ctx, 0);
    s.discover_stopping = false;
    s.discover_status = kDiscoverInProgress;
    rt.memory().store32(s.discover_params + kDiscoverResultOffset, static_cast<std::uint32_t>(kDiscoverNoPeer));
    ret(ctx, 0);
}
void DiscoverUpdate(Runtime &rt, Context &ctx) {
    NetState &s = net(rt);
    if (s.discover_status == kDiscoverInProgress && s.discover_stopping) {
        s.discover_status = kDiscoverCompleted;
        if (s.discover_params != 0) rt.memory().store32(s.discover_params + kDiscoverResultOffset, static_cast<std::uint32_t>(kDiscoverCanceled));
    }
    ret(ctx, 0);
}
void DiscoverGetStatus(Runtime &rt, Context &ctx) { ret(ctx, static_cast<std::uint32_t>(net(rt).discover_status)); }
void DiscoverTerm(Runtime &rt, Context &ctx) {
    NetState &s = net(rt);
    s.discover_status = kDiscoverNone;
    s.discover_stopping = false;
    ret(ctx, 0);
}
void DiscoverRequestSuspend(Runtime &rt, Context &ctx) {
    net(rt).discover_stopping = true;
    ret(ctx, 0);
}

const HleFunction kFunctions[] = {
    {"sceNet", 0x39AF39A6u, "sceNetInit", &NetInit},
    {"sceNet", 0x281928A9u, "sceNetTerm", &NetTerm},
    {"sceNet", 0x50647530u, "sceNetFreeThreadinfo", &NetFreeThreadinfo},
    {"sceNet", 0x0BF0A3AEu, "sceNetGetLocalEtherAddr", &NetGetLocalEtherAddr},
    {"sceWlanDrv", 0x0C622081u, "sceWlanGetEtherAddr", &WlanGetEtherAddr},
    {"sceWlanDrv", 0xD7763699u, "sceWlanGetSwitchState", &WlanGetSwitchState},
    {"sceNetApctl", 0xE2F91F9Bu, "sceNetApctlInit", &ApctlOk},
    {"sceNetApctl", 0xB3EDD0ECu, "sceNetApctlTerm", &ApctlOk},
    {"sceNetApctl", 0x8ABADD51u, "sceNetApctlAddHandler", &ApctlOk},
    {"sceNetApctl", 0x5963991Bu, "sceNetApctlDelHandler", &ApctlOk},
    {"sceNetApctl", 0x24FE91A1u, "sceNetApctlDisconnect", &ApctlOk},
    {"sceNetApctl", 0x2BEFDF23u, "sceNetApctlGetInfo", &ApctlGetInfo},
    {"sceNetInet", 0x17943399u, "sceNetInetInit", &ApctlOk},
    {"sceNetInet", 0xA9ED66B9u, "sceNetInetTerm", &ApctlOk},
    {"sceNetInet", 0x8B7B220Fu, "sceNetInetSocket", &InetFail},
    {"sceNetInet", 0x1A33F9AEu, "sceNetInetBind", &InetFail},
    {"sceNetInet", 0x2FE71FE7u, "sceNetInetSetsockopt", &InetFail},
    {"sceNetInet", 0x410B34AAu, "sceNetInetConnect", &InetFail},
    {"sceNetInet", 0x7AA671BCu, "sceNetInetSend", &InetFail},
    {"sceNetInet", 0xCDA85C99u, "sceNetInetRecv", &InetFail},
    {"sceNetInet", 0x805502DDu, "sceNetInetCloseWithRST", &InetFail},
    {"sceNetInet", 0x8D7284EAu, "sceNetInetClose", &InetFail},
    {"sceNetInet", 0xD10A1A7Au, "sceNetInetListen", &InetFail},
    {"sceNetInet", 0xDB094E1Bu, "sceNetInetAccept", &InetFail},
    {"sceNetInet", 0xE247B6D6u, "sceNetInetGetpeername", &InetFail},
    {"sceNetInet", 0xB75D5B0Au, "sceNetInetInetAddr", &InetFail},
    {"sceNetInet", 0xB3888AD4u, "sceNetInetGetTcpcbstat", &InetFail},
    {"sceNetInet", 0x8CA3A97Eu, "sceNetInetGetPspError", &InetFail},
    {"sceNetAdhoc", 0xE1D621D7u, "sceNetAdhocInit", &AdhocInit},
    {"sceNetAdhoc", 0xA62C6F57u, "sceNetAdhocTerm", &AdhocTerm},
    {"sceNetAdhocctl", 0xE26F226Eu, "sceNetAdhocctlInit", &AdhocctlInit},
    {"sceNetAdhocctl", 0x9D689E13u, "sceNetAdhocctlTerm", &AdhocctlTerm},
    {"sceNetAdhocctl", 0x20B317A0u, "sceNetAdhocctlAddHandler", &AdhocctlAddHandler},
    {"sceNetAdhocctl", 0x6402490Bu, "sceNetAdhocctlDelHandler", &AdhocctlDelHandler},
    {"sceNetAdhocctl", 0x34401D65u, "sceNetAdhocctlDisconnect", &AdhocctlDisconnect},
    {"sceNetAdhocctl", 0x08FFF7A0u, "sceNetAdhocctlScan", &AdhocctlScan},
    {"sceNetAdhocctl", 0x81AEE1BEu, "sceNetAdhocctlGetScanInfo", &AdhocctlGetScanInfo},
    {"sceNetAdhocctl", 0xE162CB14u, "sceNetAdhocctlGetPeerList", &AdhocctlGetPeerList},
    {"sceNetAdhocDiscover", 0x941B3877u, "sceNetAdhocDiscoverInitStart", &DiscoverInitStart},
    {"sceNetAdhocDiscover", 0x52DE1B97u, "sceNetAdhocDiscoverUpdate", &DiscoverUpdate},
    {"sceNetAdhocDiscover", 0x944DDBC6u, "sceNetAdhocDiscoverGetStatus", &DiscoverGetStatus},
    {"sceNetAdhocDiscover", 0xA2246614u, "sceNetAdhocDiscoverTerm", &DiscoverTerm},
    {"sceNetAdhocDiscover", 0xA423A21Bu, "sceNetAdhocDiscoverRequestSuspend", &DiscoverRequestSuspend},
};

} // namespace

std::span<const HleFunction> net_functions() { return kFunctions; }

NetState &net_state(Runtime &rt) { return net(rt); }
void net_apply_server(NetState &s) { apply_server(s); }

bool net_tracing() {
    static const bool on = [] {
        const char *v = std::getenv("MHF_TRACE_ADHOC");
        return v != nullptr && (v[0] == '1' || v[0] == '2');
    }();
    return on;
}

bool net_tracing_packets() {
    static const bool on = [] {
        const char *v = std::getenv("MHF_TRACE_ADHOC");
        return v != nullptr && v[0] == '2';
    }();
    return on;
}

void net_trace_packet(const char *direction, double seconds, int socket, const std::uint8_t *data, std::uint32_t length) {
    char text[3 * 64 + 1];
    std::size_t n = 0;
    for (std::uint32_t i = 0; i < length && i < 64; ++i) n += static_cast<std::size_t>(std::snprintf(text + n, sizeof(text) - n, "%02X ", data[i]));
    text[n] = 0;
    std::fprintf(stderr, "[packet] %.3f %s socket %d len %u: %s\n", seconds, direction, socket, length, text);
}

void net_release(Runtime &rt) { g_net.erase(&rt); }

void net_trace(const char *line) { NET_TRACE("%s", line); }

// What the network configuration dialog needs (hle_utility.cpp).
adhoc::Adhocctl &net_adhocctl(Runtime &rt) {
    NetState &s = net(rt);
    start_link(s);
    return *s.adhocctl;
}

} // namespace mhf::psp::hle

namespace mhf::psp {

void Runtime::set_adhoc_room(std::string_view code) {
    hle::NetState &s = hle::net_state(*this);
    s.room = adhoc::normalize_room_code(code);
    s.adhocctl->set_room(s.room);
    NET_TRACE("room changed: %s", s.room.empty() ? "public halls" : s.room.c_str());
}

std::string Runtime::adhoc_room() { return hle::net_state(*this).room; }

void Runtime::set_adhoc_server(std::string_view server) {
    hle::NetState &s = hle::net_state(*this);
    const std::string text(server);
    if (text == (s.server_pending ? s.next_server : s.server)) return;
    s.next_server = text;
    s.server_pending = text != s.server;
    NET_TRACE("server chosen: %s%s", text.c_str(), s.server_pending ? " (applies once the game is in no hall)" : "");
    hle::net_apply_server(s);
}

std::string Runtime::adhoc_server() {
    const hle::NetState &s = hle::net_state(*this);
    return s.server_pending ? s.next_server : s.server;
}

bool Runtime::adhoc_server_pending() { return hle::net_state(*this).server_pending; }

bool Runtime::adhoc_in_hall() { return hle::net_state(*this).adhocctl->state() == adhoc::State::Connected; }

std::string adhoc_room_code(std::string_view text) { return adhoc::normalize_room_code(text); }

std::string new_adhoc_room_code() {
    static const char *alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    std::random_device random;
    std::string code;
    for (int i = 0; i < 8; ++i) code.push_back(alphabet[random() & 31u]);
    return code;
}

std::string format_adhoc_room_code(const std::string &code) {
    return code.size() == 8 ? code.substr(0, 4) + "-" + code.substr(4) : code;
}

} // namespace mhf::psp
