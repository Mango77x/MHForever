#include "adhoc_link_aemu.hpp"

#include "adhocctl.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace mhf::psp::adhoc {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint16_t kDefaultPort = 27312;
constexpr auto kPollInterval = std::chrono::milliseconds(5);
constexpr auto kPingInterval = std::chrono::seconds(2); // the server drops a client silent for 30 s
constexpr auto kRetryInterval = std::chrono::seconds(4);

// IPv4 address of `host`, network order. False when it does not resolve.
bool resolve_ipv4(const std::string &host, std::uint32_t &address) {
#if defined(_WIN32)
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) return false;
    address = reinterpret_cast<const sockaddr_in *>(found->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(found);
    return true;
}

} // namespace

bool AemuLink::split_address(const std::string &address, std::string &host, std::uint16_t &port) {
    if (address.empty()) return false;
    const auto colon = address.rfind(':');
    host = address;
    port = kDefaultPort;
    if (colon == std::string::npos) return true;
    host = address.substr(0, colon);
    if (host.empty() || colon + 1 >= address.size()) return false;
    unsigned long value = 0;
    for (std::size_t i = colon + 1; i < address.size(); ++i) {
        if (address[i] < '0' || address[i] > '9') return false;
        value = value * 10 + static_cast<unsigned long>(address[i] - '0');
        if (value > 65535) return false;
    }
    if (value == 0) return false;
    port = static_cast<std::uint16_t>(value);
    return true;
}

AemuLink::AemuLink(Config config) : config_(std::move(config)) { thread_ = std::thread([this] { run(); }); }

AemuLink::~AemuLink() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void AemuLink::request_connect(const GroupName &group) {
    std::lock_guard lock(mutex_);
    Command c;
    c.kind = Command::Kind::Connect;
    c.group = group;
    commands_.push_back(c);
}

void AemuLink::request_disconnect() {
    std::lock_guard lock(mutex_);
    Command c;
    c.kind = Command::Kind::Disconnect;
    commands_.push_back(c);
}

void AemuLink::request_scan() {
    std::lock_guard lock(mutex_);
    Command c;
    c.kind = Command::Kind::Scan;
    commands_.push_back(c);
}

std::vector<LinkEvent> AemuLink::take_events() {
    std::lock_guard lock(mutex_);
    std::vector<LinkEvent> out;
    out.swap(events_);
    return out;
}

bool AemuLink::open_session(void *&session) {
    std::string host;
    std::uint16_t port = kDefaultPort;
    std::uint32_t ip = 0;
    if (!split_address(config_.host, host, port) || !resolve_ipv4(host, ip)) {
        std::fprintf(stderr, "[net] cannot find the ad hoc server \"%s\"\n", config_.host.c_str());
        return false;
    }
    adhocctl_addr_v4 address{};
    address.ip = ip;
    address.port = htons(port);
    char product[9] = {}; // nine characters and no terminator: the field of the protocol
    char nickname[128] = {};
    char mac[6];
    std::memcpy(product, config_.product_code.c_str(), std::min<std::size_t>(config_.product_code.size(), sizeof(product)));
    std::memcpy(nickname, config_.nickname.c_str(), std::min<std::size_t>(config_.nickname.size(), 127));
    std::memcpy(mac, config_.mac.data(), 6);
    session = create_adhocctl_v1_session_v4(
        &address, nullptr, product, nickname, mac, false, [](void *flag) { return static_cast<std::atomic<bool> *>(flag)->load(); },
        &stop_);
    if (session == nullptr) {
        std::fprintf(stderr, "[net] cannot log in to the ad hoc server %s:%u\n", host.c_str(), port);
        return false;
    }
    std::fprintf(stderr, "[net] logged in to the ad hoc server %s:%u as \"%s\"\n", host.c_str(), port, config_.nickname.c_str());
    return true;
}

void AemuLink::run() {
    void *session = nullptr;
    auto next_attempt = Clock::now();
    auto last_ping = Clock::now();
    const auto push = [this](LinkEvent e) {
        std::lock_guard lock(mutex_);
        events_.push_back(std::move(e));
    };
    while (!stop_) {
        if (session == nullptr) {
            if (Clock::now() < next_attempt) {
                std::this_thread::sleep_for(kPollInterval);
                continue;
            }
            if (!open_session(session)) {
                session = nullptr;
                next_attempt = Clock::now() + kRetryInterval;
                continue;
            }
            online_ = true;
            last_ping = Clock::now();
        }

        // What the game asked for.
        std::deque<Command> commands;
        {
            std::lock_guard lock(mutex_);
            commands.swap(commands_);
        }
        bool lost = false;
        for (const Command &c : commands) {
            adhocctl_call_status status = ADHOCCTL_CALL_SUCCESS;
            switch (c.kind) {
            case Command::Kind::Connect: status = adhocctl_connect(session, c.group.data()); break;
            case Command::Kind::Disconnect: status = adhocctl_disconnect(session); break;
            case Command::Kind::Scan: status = adhocctl_scan(session); break;
            }
            if (status == ADHOCCTL_CALL_ERROR) lost = true;
        }

        // What the server says.
        for (int budget = 0; budget < 64 && !lost; ++budget) {
            adhocctl_event event{};
            adhocctl_get_event(session, &event);
            if (event.type == ADHOCCTL_CLIENT_EVENT_WOULD_BLOCK) break;
            LinkEvent e;
            switch (event.type) {
            case ADHOCCTL_CLIENT_EVENT_SCAN:
                e.kind = LinkEvent::Kind::Scan;
                std::memcpy(e.group.data(), event.scan.group_name, 8);
                std::memcpy(e.mac.data(), event.scan.leader_mac, 6);
                break;
            case ADHOCCTL_CLIENT_EVENT_SCAN_COMPLETE: e.kind = LinkEvent::Kind::ScanComplete; break;
            case ADHOCCTL_CLIENT_EVENT_CONNECT:
                e.kind = LinkEvent::Kind::Connect;
                std::memcpy(e.mac.data(), event.connect.mac, 6);
                e.id = event.connect.id;
                e.nickname.assign(event.connect.nickname, strnlen(event.connect.nickname, sizeof(event.connect.nickname)));
                break;
            case ADHOCCTL_CLIENT_EVENT_LEADER_MAC:
                e.kind = LinkEvent::Kind::LeaderMac;
                std::memcpy(e.mac.data(), event.leader_mac.mac, 6);
                break;
            case ADHOCCTL_CLIENT_EVENT_DISCONNECT:
                e.kind = LinkEvent::Kind::Disconnect;
                e.id = event.disconnect.id;
                break;
            case ADHOCCTL_CLIENT_EVENT_CHAT: e.kind = LinkEvent::Kind::Chat; break;
            default: lost = true; continue;
            }
            push(std::move(e));
        }

        if (!lost && Clock::now() - last_ping >= kPingInterval) {
            last_ping = Clock::now();
            if (adhocctl_ping(session) == ADHOCCTL_CALL_ERROR) lost = true;
        }
        if (lost) {
            std::fprintf(stderr, "[net] lost the ad hoc server\n");
            destroy_adhocctl_session(session);
            session = nullptr;
            online_ = false;
            LinkEvent e;
            e.kind = LinkEvent::Kind::Error;
            push(std::move(e));
            next_attempt = Clock::now() + kRetryInterval;
            continue;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    if (session != nullptr) destroy_adhocctl_session(session);
    online_ = false;
}

} // namespace mhf::psp::adhoc
