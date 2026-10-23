#include "adhoc_sockets.hpp"

#include "adhoc_link_aemu.hpp"

#include "postoffice_client.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace mhf::psp::adhoc {

namespace {

constexpr auto kLoginRetry = std::chrono::seconds(1);

bool aborted(void *flag) { return static_cast<std::atomic<bool> *>(flag)->load(); }

// A port as the other players see it: the game's number plus the offset PPSSPP uses (0 stays 0).
std::uint16_t to_wire(std::uint16_t port) {
    if (port == 0) return 0;
    const auto wire = static_cast<std::uint16_t>(port + kWirePortOffset);
    return wire == 0 ? 65535 : wire;
}
std::uint16_t from_wire(int port) { return static_cast<std::uint16_t>(static_cast<std::uint16_t>(port) - kWirePortOffset); }

bool is_zero(const Mac &mac) {
    return std::all_of(mac.begin(), mac.end(), [](std::uint8_t b) { return b == 0; });
}

} // namespace

struct Sockets::Socket {
    enum class Kind { Pdp, PtpClient, PtpListen, PtpAccepted } kind = Kind::Pdp;
    int id = 0;
    Mac local{};
    std::uint16_t local_port = 0; // the game's numbering
    Mac peer{};
    std::uint16_t peer_port = 0;
    std::atomic<void *> handle{nullptr}; // the aemu client's socket, once the relay has it
    std::atomic<bool> abort{false};
    PtpState state = PtpState::Closed;

    // The login (PDP, listening) or the pairing (connecting) in progress, on a thread of its own.
    std::thread worker;
    std::atomic<int> job{0}; // 0 none, 1 running, 2 finished
    Clock::time_point retry_at{};

    // The pairing of an incoming connection (listening sockets).
    std::thread accept_worker;
    std::atomic<int> accept_job{0};
    void *accepted_handle = nullptr;
    Mac accepted_mac{};
    int accepted_port = 0;
};

Sockets::Sockets(Config config) : config_(std::move(config)) { library_up_ = init_aemu_postoffice(false) == 0; }

Sockets::~Sockets() {
    close_all();
    if (library_up_) deinit_aemu_postoffice();
}

std::shared_ptr<Sockets::Socket> Sockets::find(int id) const {
    if (id < 1 || id > kMaxSockets) return nullptr;
    std::lock_guard lock(mutex_);
    return table_[static_cast<std::size_t>(id - 1)];
}

int Sockets::allocate(std::shared_ptr<Socket> socket) {
    std::lock_guard lock(mutex_);
    for (int i = 0; i < kMaxSockets; ++i) {
        if (!table_[static_cast<std::size_t>(i)]) {
            socket->id = i + 1;
            table_[static_cast<std::size_t>(i)] = std::move(socket);
            return i + 1;
        }
    }
    return 0;
}

void Sockets::release(int id) {
    std::lock_guard lock(mutex_);
    if (id >= 1 && id <= kMaxSockets) table_[static_cast<std::size_t>(id - 1)].reset();
}

bool Sockets::pdp_port_in_use(std::uint16_t port) const {
    std::lock_guard lock(mutex_);
    return std::any_of(table_.begin(), table_.end(), [port](const std::shared_ptr<Socket> &s) {
        return s && s->kind == Socket::Kind::Pdp && s->local_port == port;
    });
}

std::uint16_t Sockets::ephemeral_port() const {
    static std::mt19937 rng{std::random_device{}()};
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto port = static_cast<std::uint16_t>(40000 + rng() % 10000);
        if (!pdp_port_in_use(port)) return port;
    }
    return 40000;
}

// ---------------------------------------------------------------- jobs

namespace {

// Joins a finished job's thread; false while it still runs.
bool reap(std::thread &thread, std::atomic<int> &job) {
    if (job == 1) return false;
    if (thread.joinable()) thread.join();
    job = 0;
    return true;
}

} // namespace

void Sockets::start_pdp_login(Socket &s) {
    if (s.handle != nullptr || Clock::now() < s.retry_at || !reap(s.worker, s.job)) return;
    s.job = 1;
    s.worker = std::thread([this, &s] {
        std::uint32_t ip = 0;
        if (AemuLink::resolve_ipv4(config_.host, ip)) {
            aemu_postoffice_sock_addr address{};
            address.addr = ip;
            address.port = htons(config_.relay_port);
            int state = 0;
            s.handle = ::pdp_create_v4(&address, reinterpret_cast<const char *>(s.local.data()), to_wire(s.local_port), false, false, &aborted,
                                     &s.abort, &state);
        }
        if (s.handle == nullptr) s.retry_at = Clock::now() + kLoginRetry;
        s.job = 2;
    });
}

void Sockets::start_listen_login(Socket &s) {
    if (s.handle != nullptr || Clock::now() < s.retry_at || !reap(s.worker, s.job)) return;
    s.job = 1;
    s.worker = std::thread([this, &s] {
        std::uint32_t ip = 0;
        if (AemuLink::resolve_ipv4(config_.host, ip)) {
            aemu_postoffice_sock_addr address{};
            address.addr = ip;
            address.port = htons(config_.relay_port);
            int state = 0;
            s.handle = ::ptp_listen_v4(&address, reinterpret_cast<const char *>(s.local.data()), to_wire(s.local_port), false, &aborted, &s.abort,
                                     &state);
        }
        if (s.handle == nullptr) s.retry_at = Clock::now() + kLoginRetry;
        s.job = 2;
    });
}

void Sockets::start_ptp_connect(Socket &s) {
    s.job = 1;
    s.worker = std::thread([this, &s] {
        std::uint32_t ip = 0;
        if (AemuLink::resolve_ipv4(config_.host, ip)) {
            aemu_postoffice_sock_addr address{};
            address.addr = ip;
            address.port = htons(config_.relay_port);
            int state = 0;
            s.handle = ::ptp_connect_v4(&address, reinterpret_cast<const char *>(s.local.data()), to_wire(s.local_port),
                                      reinterpret_cast<const char *>(s.peer.data()), to_wire(s.peer_port), false, &aborted, &s.abort, &state);
        }
        s.job = 2;
    });
}

void Sockets::shut(Socket &s) {
    s.abort = true;
    if (s.worker.joinable()) s.worker.join();
    if (s.accept_worker.joinable()) s.accept_worker.join();
    if (s.accepted_handle != nullptr) {
        ::ptp_close(s.accepted_handle);
        s.accepted_handle = nullptr;
    }
    void *handle = s.handle.exchange(nullptr);
    if (handle != nullptr) {
        switch (s.kind) {
        case Socket::Kind::Pdp: ::pdp_delete(handle); break;
        case Socket::Kind::PtpListen: ::ptp_listen_close(handle); break;
        default: ::ptp_close(handle); break;
        }
    }
    s.state = PtpState::Closed;
}

void Sockets::close_all() {
    for (int id = 1; id <= kMaxSockets; ++id) {
        if (auto s = find(id)) {
            shut(*s);
            release(id);
        }
    }
}

// ---------------------------------------------------------------- PDP

std::int64_t Sockets::pdp_create(std::uint16_t port) {
    if (!library_up_) return as_status(sockerr::kNoSpace);
    if (port != 0 && pdp_port_in_use(port)) return as_status(sockerr::kPortInUse);
    auto s = std::make_shared<Socket>();
    s->kind = Socket::Kind::Pdp;
    s->local = config_.mac;
    s->local_port = port != 0 ? port : ephemeral_port();
    const int id = allocate(s);
    if (id == 0) return as_status(sockerr::kNoSpace);
    start_pdp_login(*s);
    return id;
}

std::uint32_t Sockets::pdp_delete(int id) {
    const auto s = find(id);
    if (!s || s->kind != Socket::Kind::Pdp) return sockerr::kInvalidSocketId;
    shut(*s);
    release(id);
    return 0;
}

std::uint32_t Sockets::pdp_send(int id, const Mac &destination, std::uint16_t port, const std::uint8_t *data, std::uint32_t length) {
    const auto s = find(id);
    if (!s || s->kind != Socket::Kind::Pdp) return sockerr::kInvalidSocketId;
    if (port == 0) return sockerr::kInvalidPort;
    if (is_zero(destination)) return sockerr::kInvalidAddr;
    void *handle = s->handle;
    if (handle == nullptr) {
        start_pdp_login(*s);
        return sockerr::kWouldBlock;
    }
    const int result = ::pdp_send(handle, reinterpret_cast<const char *>(destination.data()), to_wire(port),
                                reinterpret_cast<const char *>(data), static_cast<int>(length), true);
    if (result == AEMU_POSTOFFICE_CLIENT_OK) return 0;
    if (result == AEMU_POSTOFFICE_CLIENT_SESSION_DEAD) {
        // The relay dropped the socket: log it in again and let the game try once more.
        void *dead = s->handle.exchange(nullptr);
        if (dead != nullptr) ::pdp_delete(dead);
        start_pdp_login(*s);
    }
    return sockerr::kWouldBlock;
}

std::uint32_t Sockets::pdp_recv(int id, Mac &source, std::uint16_t &port, std::uint8_t *buffer, std::uint32_t &length) {
    const auto s = find(id);
    if (!s || s->kind != Socket::Kind::Pdp) return sockerr::kInvalidSocketId;
    void *handle = s->handle;
    if (handle == nullptr) {
        start_pdp_login(*s);
        return sockerr::kWouldBlock;
    }
    const int next = ::pdp_peek_next_size(handle);
    if (next == AEMU_POSTOFFICE_CLIENT_SESSION_DEAD) {
        void *dead = s->handle.exchange(nullptr);
        if (dead != nullptr) ::pdp_delete(dead);
        start_pdp_login(*s);
        return sockerr::kWouldBlock;
    }
    if (next <= 0) return sockerr::kWouldBlock;
    if (static_cast<std::uint32_t>(next) > length) {
        length = static_cast<std::uint32_t>(next);
        return sockerr::kNotEnoughSpace;
    }
    int received = static_cast<int>(length);
    int from_port = 0;
    char mac[6] = {};
    const int result = ::pdp_recv(handle, mac, &from_port, reinterpret_cast<char *>(buffer), &received, true);
    if (result != AEMU_POSTOFFICE_CLIENT_OK) return sockerr::kWouldBlock;
    std::memcpy(source.data(), mac, 6);
    port = from_wire(from_port);
    length = static_cast<std::uint32_t>(received);
    return 0;
}

// ---------------------------------------------------------------- PTP

std::int64_t Sockets::ptp_open(std::uint16_t source_port, const Mac &destination, std::uint16_t destination_port) {
    if (!library_up_) return as_status(sockerr::kNoSpace);
    auto s = std::make_shared<Socket>();
    s->kind = Socket::Kind::PtpClient;
    s->local = config_.mac;
    s->local_port = source_port != 0 ? source_port : ephemeral_port();
    s->peer = destination;
    s->peer_port = destination_port;
    const int id = allocate(s);
    return id != 0 ? id : as_status(sockerr::kNoSpace);
}

std::int64_t Sockets::ptp_listen(std::uint16_t source_port) {
    if (!library_up_) return as_status(sockerr::kNoSpace);
    {
        std::lock_guard lock(mutex_);
        for (const auto &other : table_) {
            if (other && other->kind == Socket::Kind::PtpListen && other->local_port == source_port && source_port != 0) return as_status(sockerr::kPortInUse);
        }
    }
    auto s = std::make_shared<Socket>();
    s->kind = Socket::Kind::PtpListen;
    s->local = config_.mac;
    s->local_port = source_port != 0 ? source_port : ephemeral_port();
    s->state = PtpState::Listen;
    const int id = allocate(s);
    if (id == 0) return as_status(sockerr::kNoSpace);
    start_listen_login(*s);
    return id;
}

std::uint32_t Sockets::ptp_connect(int id) {
    const auto s = find(id);
    if (!s || s->kind != Socket::Kind::PtpClient) return s ? sockerr::kNotOpened : sockerr::kInvalidSocketId;
    if (s->state == PtpState::Established) return 0;
    if (s->job == 1) return sockerr::kWouldBlock;
    if (s->job == 2) {
        (void)reap(s->worker, s->job);
        if (s->handle != nullptr) {
            s->state = PtpState::Established;
            return 0;
        }
        s->state = PtpState::Closed; // nobody was listening there (yet): the game decides whether to try again
        return sockerr::kConnectionRefused;
    }
    s->state = PtpState::SynSent;
    start_ptp_connect(*s);
    return sockerr::kWouldBlock;
}

std::int64_t Sockets::ptp_accept(int id, Mac &peer, std::uint16_t &port) {
    const auto s = find(id);
    if (!s || s->kind != Socket::Kind::PtpListen) return as_status(s ? sockerr::kNotListened : sockerr::kInvalidSocketId);
    void *listener = s->handle;
    if (listener == nullptr) {
        start_listen_login(*s);
        return as_status(sockerr::kWouldBlock);
    }
    if (s->accept_job == 2) {
        (void)reap(s->accept_worker, s->accept_job);
        void *accepted = s->accepted_handle;
        s->accepted_handle = nullptr;
        if (accepted == nullptr) return as_status(sockerr::kWouldBlock);
        auto connection = std::make_shared<Socket>();
        connection->kind = Socket::Kind::PtpAccepted;
        connection->local = s->local;
        connection->local_port = s->local_port;
        connection->peer = s->accepted_mac;
        connection->peer_port = from_wire(s->accepted_port);
        connection->handle = accepted;
        connection->state = PtpState::Established;
        const int new_id = allocate(connection);
        if (new_id == 0) {
            ::ptp_close(accepted);
            connection->handle = nullptr;
            return as_status(sockerr::kNoSpace);
        }
        peer = connection->peer;
        port = connection->peer_port;
        return new_id;
    }
    if (s->accept_job == 1) return as_status(sockerr::kWouldBlock);
    if (::ptp_listen_has_request(listener) > 0) {
        s->accept_job = 1;
        s->accept_worker = std::thread([&socket = *s, listener] {
            int state = 0;
            int from_port = 0;
            char mac[6] = {};
            socket.accepted_handle = ::ptp_accept(listener, mac, &from_port, true, &aborted, &socket.abort, &state);
            std::memcpy(socket.accepted_mac.data(), mac, 6);
            socket.accepted_port = from_port;
            socket.accept_job = 2;
        });
    }
    return as_status(sockerr::kWouldBlock);
}

std::uint32_t Sockets::ptp_send(int id, const std::uint8_t *data, std::uint32_t &length) {
    const auto s = find(id);
    if (!s || s->kind == Socket::Kind::Pdp || s->kind == Socket::Kind::PtpListen) return s ? sockerr::kNotConnected : sockerr::kInvalidSocketId;
    if (s->state != PtpState::Established && s->state != PtpState::SynSent) return sockerr::kNotConnected;
    void *handle = s->handle;
    if (handle == nullptr) return sockerr::kWouldBlock; // the pairing is still going on
    const std::uint32_t chunk = std::min<std::uint32_t>(length, AEMU_POSTOFFICE_PTP_BLOCK_MAX);
    const int result = ::ptp_send(handle, reinterpret_cast<const char *>(data), static_cast<int>(chunk), true);
    if (result == AEMU_POSTOFFICE_CLIENT_OK) {
        length = chunk;
        return 0;
    }
    if (result == AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK) return sockerr::kWouldBlock;
    s->state = PtpState::Closed;
    return sockerr::kDisconnected;
}

std::uint32_t Sockets::ptp_recv(int id, std::uint8_t *buffer, std::uint32_t &length) {
    const auto s = find(id);
    if (!s || s->kind == Socket::Kind::Pdp || s->kind == Socket::Kind::PtpListen) return s ? sockerr::kNotConnected : sockerr::kInvalidSocketId;
    if (s->state != PtpState::Established && s->state != PtpState::SynSent) return sockerr::kNotConnected;
    void *handle = s->handle;
    if (handle == nullptr) return sockerr::kWouldBlock;
    int received = static_cast<int>(length);
    const int result = ::ptp_recv(handle, reinterpret_cast<char *>(buffer), &received, true);
    if (result == AEMU_POSTOFFICE_CLIENT_OK) {
        length = static_cast<std::uint32_t>(received);
        return 0;
    }
    if (result == AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK) return sockerr::kWouldBlock;
    s->state = PtpState::Closed;
    return sockerr::kDisconnected;
}

std::uint32_t Sockets::ptp_flush(int id) {
    const auto s = find(id);
    if (!s || s->kind == Socket::Kind::Pdp || s->kind == Socket::Kind::PtpListen) return s ? sockerr::kNotConnected : sockerr::kInvalidSocketId;
    void *handle = s->handle;
    if (handle == nullptr || s->state != PtpState::Established) return 0;
    const int room = ::ptp_send_buf_not_full(handle);
    if (room == 1) return 0;
    return room == 0 ? sockerr::kWouldBlock : sockerr::kDisconnected;
}

std::uint32_t Sockets::ptp_close(int id) {
    const auto s = find(id);
    if (!s || s->kind == Socket::Kind::Pdp) return sockerr::kInvalidSocketId;
    shut(*s);
    release(id);
    return 0;
}

std::vector<PtpStat> Sockets::ptp_stats() {
    std::vector<PtpStat> out;
    for (int id = 1; id <= kMaxSockets; ++id) {
        const auto s = find(id);
        if (!s || s->kind == Socket::Kind::Pdp) continue;
        PtpStat stat;
        stat.id = id;
        stat.local = s->local;
        stat.peer = s->peer;
        stat.local_port = s->local_port;
        stat.peer_port = s->peer_port;
        stat.state = s->state;
        void *handle = s->handle;
        if (handle != nullptr && s->kind != Socket::Kind::PtpListen) {
            const int available = ::ptp_peek_next_size(handle);
            stat.recv_available = available > 0 ? static_cast<std::uint32_t>(available) : 0;
            if (s->state == PtpState::SynSent) s->state = stat.state = PtpState::Established;
        }
        out.push_back(stat);
    }
    return out;
}

} // namespace mhf::psp::adhoc
