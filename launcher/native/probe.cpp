#include "probe.hpp"

#include <chrono>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mhf::launcher {

namespace {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void close_socket(Socket s) { closesocket(s); }
bool set_nonblocking(Socket s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}
bool connect_pending() { return WSAGetLastError() == WSAEWOULDBLOCK; }
// Winsock wants WSAStartup once per process; it counts, so pairing one with each probe is fine.
struct Winsock {
    bool ok;
    Winsock() {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~Winsock() {
        if (ok) WSACleanup();
    }
};
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void close_socket(Socket s) { close(s); }
bool set_nonblocking(Socket s) { return fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) == 0; }
bool connect_pending() { return errno == EINPROGRESS; }
struct Winsock {
    bool ok = true;
};
#endif

// Waits until `s` finishes connecting; true when it connected.
bool wait_connected(Socket s, int timeout_ms) {
#if defined(_WIN32)
    WSAPOLLFD p{s, POLLWRNORM, 0};
    if (WSAPoll(&p, 1, timeout_ms) != 1) return false;
#else
    pollfd p{s, POLLOUT, 0};
    if (poll(&p, 1, timeout_ms) != 1) return false;
#endif
    int error = 0;
    socklen_t size = sizeof error;
    return getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size) == 0 && error == 0;
}

} // namespace

std::optional<int> probe_server(const std::string &server, int timeout_ms) {
    const Winsock winsock;
    if (!winsock.ok) return std::nullopt;
    const std::size_t colon = server.find(':');
    const std::string host = server.substr(0, colon);
    const std::string port = colon == std::string::npos ? std::to_string(kAdhocctlPort) : server.substr(colon + 1);
    if (host.empty()) return std::nullopt;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *found = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0) return std::nullopt;
    // Only connecting is timed (each address on its own): resolving the name can be slow without the server being far.
    const auto start = std::chrono::steady_clock::now();
    std::optional<int> result;
    for (const addrinfo *a = found; a != nullptr && !result; a = a->ai_next) {
        const Socket s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == kNoSocket) continue;
        const auto attempt = std::chrono::steady_clock::now();
        const int left = timeout_ms - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - start).count());
        if (left > 0 && set_nonblocking(s) &&
            (connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0 || (connect_pending() && wait_connected(s, left)))) {
            result = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - attempt).count());
        }
        close_socket(s);
    }
    freeaddrinfo(found);
    return result;
}

} // namespace mhf::launcher
