#pragma once

// The ad hoc sockets of the PSP (sceNetAdhoc PDP datagrams and PTP streams) over the relay of a PSP ad hoc server, through
// the aemu_postoffice client. Nothing here blocks: what takes time (logging in a socket to the relay, pairing a PTP
// connection with the other player's listening socket) runs on a thread of its own and the calls report "would block" until
// it is done, so the HLE can wait the way the PSP's calls do without holding the game up. docs/08-multiplayer.md 1.2.

#include "adhocctl.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mhf::psp::adhoc {

// PPSSPP adds this to every port on the wire; both ends must, or they do not meet.
inline constexpr std::uint16_t kWirePortOffset = 10000;

// sceNetAdhoc error codes.
namespace sockerr {
inline constexpr std::uint32_t kInvalidSocketId = 0x80410701u;
inline constexpr std::uint32_t kInvalidAddr = 0x80410702u;
inline constexpr std::uint32_t kInvalidPort = 0x80410703u;
inline constexpr std::uint32_t kNotEnoughSpace = 0x80400706u;
inline constexpr std::uint32_t kWouldBlock = 0x80410709u;
inline constexpr std::uint32_t kPortInUse = 0x8041070Au;
inline constexpr std::uint32_t kNotConnected = 0x8041070Bu;
inline constexpr std::uint32_t kDisconnected = 0x8041070Cu;
inline constexpr std::uint32_t kNotOpened = 0x8040070Du;
inline constexpr std::uint32_t kNotListened = 0x8040070Eu;
inline constexpr std::uint32_t kInvalidArg = 0x80410711u;
inline constexpr std::uint32_t kTimeout = 0x80410715u;
inline constexpr std::uint32_t kConnectionRefused = 0x80410718u;
inline constexpr std::uint32_t kNoSpace = 0x80410F00u; // out of socket slots (what the PSP calls ERROR_NET_NO_SPACE)
} // namespace sockerr

// An error code as the signed result of the calls that return an id (> 0) or an error (< 0), as the PSP does.
[[nodiscard]] constexpr std::int64_t as_status(std::uint32_t code) { return static_cast<std::int32_t>(code); }

enum class PtpState : std::int32_t { Closed = 0, Listen = 1, SynSent = 2, SynRcvd = 3, Established = 4 };

struct PtpStat {
    int id{};
    Mac local{}, peer{};
    std::uint16_t local_port{}, peer_port{};
    std::uint32_t send_pending{}, recv_available{};
    PtpState state = PtpState::Closed;
};

class Sockets {
public:
    static constexpr int kMaxSockets = 255;

    struct Config {
        std::string host;             // the server's address (the relay is on `relay_port`)
        std::uint16_t relay_port{};   // TCP port of the relay
        Mac mac{};                    // this console's virtual MAC
    };

    explicit Sockets(Config config);
    ~Sockets();
    Sockets(const Sockets &) = delete;
    Sockets &operator=(const Sockets &) = delete;

    // Results are 0 / an id (> 0) on success, or one of sockerr:: (>= 0x80000000 as a 32-bit value; as_status() for the calls that
    // return an id).
    [[nodiscard]] std::int64_t pdp_create(std::uint16_t port);
    [[nodiscard]] std::uint32_t pdp_delete(int id);
    // `port` is the destination in the game's own numbering (the wire offset is added here).
    [[nodiscard]] std::uint32_t pdp_send(int id, const Mac &destination, std::uint16_t port, const std::uint8_t *data, std::uint32_t length);
    // `length` is the room in `buffer` and becomes the size of the datagram (also when it did not fit: kNotEnoughSpace).
    [[nodiscard]] std::uint32_t pdp_recv(int id, Mac &source, std::uint16_t &port, std::uint8_t *buffer, std::uint32_t &length);

    [[nodiscard]] std::int64_t ptp_open(std::uint16_t source_port, const Mac &destination, std::uint16_t destination_port);
    [[nodiscard]] std::int64_t ptp_listen(std::uint16_t source_port);
    [[nodiscard]] std::uint32_t ptp_connect(int id);                              // 0 once connected; kWouldBlock while pairing
    [[nodiscard]] std::int64_t ptp_accept(int id, Mac &peer, std::uint16_t &port); // the new socket's id, or kWouldBlock
    [[nodiscard]] std::uint32_t ptp_send(int id, const std::uint8_t *data, std::uint32_t &length);
    [[nodiscard]] std::uint32_t ptp_recv(int id, std::uint8_t *buffer, std::uint32_t &length);
    [[nodiscard]] std::uint32_t ptp_flush(int id);
    [[nodiscard]] std::uint32_t ptp_close(int id);
    [[nodiscard]] std::vector<PtpStat> ptp_stats();

    // Closes everything (the game left the ad hoc library, or the server was lost).
    void close_all();

    struct Socket;

private:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] std::shared_ptr<Socket> find(int id) const;
    [[nodiscard]] int allocate(std::shared_ptr<Socket> socket); // 0 when full
    void release(int id);
    [[nodiscard]] bool pdp_port_in_use(std::uint16_t port) const;
    [[nodiscard]] std::uint16_t ephemeral_port() const;
    void start_pdp_login(Socket &s);
    void start_listen_login(Socket &s);
    void start_ptp_connect(Socket &s);
    void shut(Socket &s);

    Config config_;
    std::uint32_t relay_ip_ = 0;           // network order; 0 until the name was resolved
    bool relay_ip_resolved_ = false;
    mutable std::mutex mutex_;             // the table; the sockets themselves are used by one guest thread at a time
    std::array<std::shared_ptr<Socket>, kMaxSockets> table_{};
    bool library_up_ = false;
};

} // namespace mhf::psp::adhoc
