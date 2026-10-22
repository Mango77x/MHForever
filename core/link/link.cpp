#include "link/link.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mhf::link {

namespace {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;

void start_sockets() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
void close_socket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket kNoSocket = -1;

void start_sockets() {}
void close_socket(Socket s) { ::close(s); }
#endif

// Waits until `s` can be read (data, a connection, or the end of the stream). False on timeout or error.
bool wait_readable(Socket s, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set); // NOLINT: the descriptor is below FD_SETSIZE on POSIX (a handful are open)
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &tv) > 0;
}

bool send_all(Socket s, const char *data, std::size_t size) {
    while (size > 0) {
#if defined(_WIN32)
        const int sent = ::send(s, data, static_cast<int>(std::min<std::size_t>(size, 1 << 20)), 0);
#else
        const ssize_t sent = ::send(s, data, size, MSG_NOSIGNAL);
#endif
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

std::string random_token() {
    std::random_device device;
    std::string token;
    for (int i = 0; i < 8; ++i) {
        char part[9];
        std::snprintf(part, sizeof part, "%08x", static_cast<unsigned>(device()));
        token += part;
    }
    return token;
}

} // namespace

// ---------------------------------------------------------------- Connection

struct Connection::Impl {
    Socket socket = kNoSocket;
    std::string pending; // bytes received that do not make a whole line yet
    std::mutex send_mutex;
    ~Impl() {
        if (socket != kNoSocket) close_socket(socket);
    }
};

Connection::Connection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Connection::~Connection() = default;

bool Connection::send(const std::string &line) {
    if (!open_ || line.empty() || line.size() > kMaxLine || line.find('\n') != std::string::npos) return false;
    std::lock_guard lock(impl_->send_mutex);
    const std::string framed = line + '\n';
    if (!send_all(impl_->socket, framed.data(), framed.size())) {
        open_ = false;
        return false;
    }
    return true;
}

std::optional<std::string> Connection::receive(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        if (const auto end = impl_->pending.find('\n'); end != std::string::npos) {
            std::string line = impl_->pending.substr(0, end);
            impl_->pending.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            return line;
        }
        if (!open_) return std::nullopt;
        if (impl_->pending.size() > kMaxLine) { // a peer that never ends a line: not one of ours
            open_ = false;
            return std::nullopt;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (!wait_readable(impl_->socket, static_cast<int>(std::max<long long>(left.count(), 0)))) return std::nullopt;
        std::array<char, 4096> buffer;
        const int got = ::recv(impl_->socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (got <= 0) {
            open_ = false; // closed or failed; whole lines already received are still handed out first
            continue;
        }
        impl_->pending.append(buffer.data(), static_cast<std::size_t>(got));
    }
}

// ---------------------------------------------------------------- Server

struct Server::Impl {
    Socket listener = kNoSocket;
    std::uint16_t port = 0;
    std::string token;
    ~Impl() {
        if (listener != kNoSocket) close_socket(listener);
    }
};

Server::Server(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Server::~Server() = default;

std::unique_ptr<Server> Server::start() {
    start_sockets();
    auto impl = std::make_unique<Impl>();
    impl->listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl->listener == kNoSocket) return nullptr;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(impl->listener, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0 ||
        ::listen(impl->listener, 4) != 0) {
        return nullptr;
    }
#if defined(_WIN32)
    int length = sizeof address;
#else
    socklen_t length = sizeof address;
#endif
    if (::getsockname(impl->listener, reinterpret_cast<sockaddr *>(&address), &length) != 0) return nullptr;
    impl->port = ntohs(address.sin_port);
    impl->token = random_token();
    return std::unique_ptr<Server>(new Server(std::move(impl)));
}

std::uint16_t Server::port() const { return impl_->port; }
const std::string &Server::token() const { return impl_->token; }
std::string Server::address() const { return std::to_string(impl_->port) + ":" + impl_->token; }

std::unique_ptr<Connection> Server::accept(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (!wait_readable(impl_->listener, static_cast<int>(std::max<long long>(left.count(), 0)))) return nullptr;
        const Socket client = ::accept(impl_->listener, nullptr, nullptr);
        if (client == kNoSocket) continue;
        const int on = 1;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof on);
        auto impl = std::make_unique<Connection::Impl>();
        impl->socket = client;
        auto connection = std::make_unique<Connection>(std::move(impl));
        // The first line must be the token; anyone else is dropped.
        const auto hello = connection->receive(1000);
        if (hello && *hello == "hello " + impl_->token) return connection;
    }
}

// ---------------------------------------------------------------- the game's end

std::unique_ptr<Connection> connect(const std::string &address) {
    const auto colon = address.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= address.size()) return nullptr;
    int port = 0;
    for (std::size_t i = 0; i < colon; ++i) {
        if (address[i] < '0' || address[i] > '9') return nullptr;
        port = port * 10 + (address[i] - '0');
        if (port > 65535) return nullptr;
    }
    const std::string token = address.substr(colon + 1);
    start_sockets();
    const Socket s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kNoSocket) return nullptr;
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    peer.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::connect(s, reinterpret_cast<sockaddr *>(&peer), sizeof peer) != 0) {
        close_socket(s);
        return nullptr;
    }
    const int on = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof on);
    auto impl = std::make_unique<Connection::Impl>();
    impl->socket = s;
    auto connection = std::make_unique<Connection>(std::move(impl));
    if (!connection->send("hello " + token)) return nullptr;
    return connection;
}

} // namespace mhf::link
