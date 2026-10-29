// The ad hoc control library against a real PSP ad hoc server (aemu_postoffice's reference server, or any other): two
// players log in, one creates a group, the other scans, finds it and joins; they see each other; one leaves.
//
// Then the sockets over the same server's relay: datagrams both ways (and one that does not fit), a stream between a listening
// socket and a connecting one, and the end of the stream when one side closes.
//
// Needs a server, so it only runs when MHF_ADHOC_TEST_SERVER is set ("host" or "host:port"); otherwise it passes at once.
// Not part of the game data tests: it uses made-up product codes and nicknames.

#include "adhoc_link_aemu.hpp"
#include "adhoc_sockets.hpp"
#include "adhocctl.hpp"

#include "check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace mhf::psp::adhoc;
using Clock = std::chrono::steady_clock;

struct Player {
    Player(const std::string &server, const char *nick, std::uint8_t last)
        : link([&] {
              AemuLink::Config c;
              c.host = server;
              c.product_code = "ULTEST001";
              c.nickname = nick;
              c.mac = Mac{0x02, 0x4D, 0x48, 0x46, 0x00, last};
              return c;
          }()),
          library(&link) {
        (void)library.init();
    }
    // Polls until `done` holds or the time runs out.
    bool wait(const std::function<bool()> &done, int seconds = 10) {
        const auto end = Clock::now() + std::chrono::seconds(seconds);
        while (Clock::now() < end) {
            events_seen += library.poll().size();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return done();
    }
    AemuLink link;
    Adhocctl library;
    std::size_t events_seen = 0;
};

GroupName group(const char *name) {
    GroupName g{};
    std::memcpy(g.data(), name, std::min<std::size_t>(std::strlen(name), g.size()));
    return g;
}

} // namespace

int main() {
    const char *server = std::getenv("MHF_ADHOC_TEST_SERVER");
    if (server == nullptr || server[0] == '\0') {
        std::printf("psp_adhocctl_server: no MHF_ADHOC_TEST_SERVER, skipped\n");
        return 0;
    }

    // The aemu client does not allow two sessions to be created at the same time (it shares a table of slots), so the second
    // player starts once the first is logged in. In the game there is one.
    Player alice(server, "Alice", 1);
    MHF_CHECK(alice.wait([&] { return alice.library.online(); }));
    Player bob(server, "Bob", 2);
    MHF_CHECK(bob.wait([&] { return bob.library.online(); }));

    // Alice creates the group by joining it.
    const std::string name = "G" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count() % 100000);
    MHF_CHECK(alice.library.connect(group(name.c_str())) == 0);
    MHF_CHECK(alice.wait([&] { return alice.library.state() == State::Connected; }));
    MHF_CHECK(alice.library.peers().empty());

    // Bob scans, finds the group, joins it.
    MHF_CHECK(bob.library.scan() == 0);
    MHF_CHECK(bob.wait([&] { return bob.library.state() == State::Disconnected && !bob.library.groups().empty(); }));
    bool found = false;
    for (const ScanGroup &g : bob.library.groups()) found = found || g.name == group(name.c_str());
    MHF_CHECK(found);
    MHF_CHECK(bob.library.connect(group(name.c_str())) == 0);
    MHF_CHECK(bob.wait([&] { return bob.library.state() == State::Connected; }));

    // They see each other.
    MHF_CHECK(bob.wait([&] { return bob.library.peers().size() == 1; }));
    MHF_CHECK(!bob.library.peers().empty() && bob.library.peers()[0].nickname == "Alice");
    MHF_CHECK(alice.wait([&] { return alice.library.peers().size() == 1; }));
    MHF_CHECK(!alice.library.peers().empty() && alice.library.peers()[0].nickname == "Bob");
    MHF_CHECK(!alice.library.peers().empty() && alice.library.peers()[0].mac == (Mac{0x02, 0x4D, 0x48, 0x46, 0x00, 2}));

    // Doing nothing keeps the connection: the server drops a client that is silent for 30 s, so the pings must be doing their job.
    if (std::getenv("MHF_ADHOC_TEST_KEEPALIVE") != nullptr) {
        const auto idle_end = Clock::now() + std::chrono::seconds(45);
        while (Clock::now() < idle_end) {
            alice.events_seen += alice.library.poll().size();
            bob.events_seen += bob.library.poll().size();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        MHF_CHECK(alice.library.state() == State::Connected && alice.library.online());
        MHF_CHECK(bob.library.state() == State::Connected && bob.library.online());
    }

    // Bob leaves; Alice notices.
    MHF_CHECK(bob.library.disconnect() == 0);
    MHF_CHECK(alice.wait([&] { return alice.library.peers().empty(); }));
    MHF_CHECK(alice.library.state() == State::Connected);

    // ---- sockets over the relay (the next port up from the server's)
    MHF_CHECK(bob.library.connect(group(name.c_str())) == 0);
    MHF_CHECK(bob.wait([&] { return bob.library.state() == State::Connected; }));
    MHF_CHECK(alice.wait([&] { return alice.library.peers().size() == 1; }));
    std::string host;
    std::uint16_t port = 0;
    MHF_CHECK(AemuLink::split_address(server, host, port));
    const Mac alice_mac{0x02, 0x4D, 0x48, 0x46, 0x00, 1}, bob_mac{0x02, 0x4D, 0x48, 0x46, 0x00, 2};
    Sockets::Config alice_config, bob_config;
    alice_config.host = bob_config.host = host;
    alice_config.relay_port = bob_config.relay_port = static_cast<std::uint16_t>(port + 1);
    alice_config.mac = alice_mac;
    bob_config.mac = bob_mac;
    Sockets alice_net(alice_config), bob_net(bob_config);
    // Retries `attempt` until it answers (not "would block") or the time runs out.
    const auto until = [](const std::function<std::uint32_t()> &attempt, int seconds = 10) {
        const auto end = Clock::now() + std::chrono::seconds(seconds);
        std::uint32_t result = sockerr::kWouldBlock;
        while (Clock::now() < end) {
            result = attempt();
            if (result != sockerr::kWouldBlock) return result;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return result;
    };

    // Datagrams.
    const auto a_pdp = alice_net.pdp_create(1000), b_pdp = bob_net.pdp_create(2000);
    MHF_CHECK(a_pdp > 0 && b_pdp > 0);
    MHF_CHECK(alice_net.pdp_create(1000) == as_status(sockerr::kPortInUse));
    // A datagram sent before the other socket exists on the relay is lost, as on any UDP: give both a moment to log in.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const std::uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
    MHF_CHECK(until([&] { return alice_net.pdp_send(static_cast<int>(a_pdp), bob_mac, 2000, hello, sizeof(hello)); }) == 0);
    std::uint8_t buffer[64] = {};
    Mac from{};
    std::uint16_t from_port = 0;
    std::uint32_t length = sizeof(buffer);
    MHF_CHECK(until([&] {
        length = sizeof(buffer);
        return bob_net.pdp_recv(static_cast<int>(b_pdp), from, from_port, buffer, length);
    }) == 0);
    MHF_CHECK(length == 5 && std::memcmp(buffer, "hello", 5) == 0 && from == alice_mac && from_port == 1000);
    // A datagram that does not fit says how big it is, and stays for a bigger buffer.
    const std::uint8_t big[40] = {1};
    MHF_CHECK(until([&] { return bob_net.pdp_send(static_cast<int>(b_pdp), alice_mac, 1000, big, sizeof(big)); }) == 0);
    std::uint8_t small[8] = {};
    length = sizeof(small);
    MHF_CHECK(until([&] {
        length = sizeof(small);
        return alice_net.pdp_recv(static_cast<int>(a_pdp), from, from_port, small, length);
    }) == sockerr::kNotEnoughSpace);
    MHF_CHECK(length == 40);
    length = sizeof(buffer);
    MHF_CHECK(alice_net.pdp_recv(static_cast<int>(a_pdp), from, from_port, buffer, length) == 0 && length == 40 && from_port == 2000);
    MHF_CHECK(alice_net.pdp_recv(static_cast<int>(a_pdp), from, from_port, buffer, length) == sockerr::kWouldBlock);

    // How even the datagrams arrive: 30 per second for 10 s from Alice to Bob, each stamped with its send time. Only with
    // MHF_ADHOC_TEST_JITTER=1; prints the delay and the gaps between arrivals (a game that sees them bunch up shows jerky movement).
    if (std::getenv("MHF_ADHOC_TEST_JITTER") != nullptr) {
        std::vector<double> delays, gaps;
        double last_arrival = 0;
        int sent_count = 0, received_count = 0;
        const auto start = Clock::now();
        const auto now_ms = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
        auto next_send = 0.0;
        while (now_ms() < 10000.0) {
            if (now_ms() >= next_send) {
                double stamp = now_ms();
                std::uint8_t packet[24] = {};
                std::memcpy(packet, &stamp, sizeof(stamp));
                if (alice_net.pdp_send(static_cast<int>(a_pdp), bob_mac, 2000, packet, sizeof(packet)) == 0) ++sent_count;
                next_send += 1000.0 / 30.0;
            }
            for (;;) {
                std::uint8_t in[64];
                std::uint32_t got = sizeof(in);
                Mac who{};
                std::uint16_t who_port = 0;
                if (bob_net.pdp_recv(static_cast<int>(b_pdp), who, who_port, in, got) != 0) break;
                double stamp = 0;
                std::memcpy(&stamp, in, sizeof(stamp));
                const double t = now_ms();
                delays.push_back(t - stamp);
                if (last_arrival > 0) gaps.push_back(t - last_arrival);
                last_arrival = t;
                ++received_count;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto stat = [](std::vector<double> v, double q) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            return v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1))];
        };
        std::printf("[jitter] sent %d, received %d; delay ms: median %.1f, 95%% %.1f, max %.1f; gap between arrivals ms: median %.1f, 95%% %.1f, max %.1f\n",
                    sent_count, received_count, stat(delays, 0.5), stat(delays, 0.95), stat(delays, 1.0), stat(gaps, 0.5), stat(gaps, 0.95), stat(gaps, 1.0));
    }

    // A stream: Bob listens, Alice opens and connects, Bob accepts; both talk; Alice closes, Bob sees it end.
    const auto listener = bob_net.ptp_listen(3000);
    MHF_CHECK(listener > 0);
    const auto client = alice_net.ptp_open(0, bob_mac, 3000);
    MHF_CHECK(client > 0);
    // The pairing needs both sides at it: Alice connects while Bob accepts.
    std::int64_t accepted = 0;
    Mac peer{};
    std::uint16_t peer_port = 0;
    bool connected = false;
    const auto end = Clock::now() + std::chrono::seconds(15);
    while (Clock::now() < end && (!connected || accepted <= 0)) {
        if (!connected) connected = alice_net.ptp_connect(static_cast<int>(client)) == 0;
        if (accepted <= 0) {
            const auto r = bob_net.ptp_accept(static_cast<int>(listener), peer, peer_port);
            if (r > 0) accepted = r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    MHF_CHECK(connected && accepted > 0);
    MHF_CHECK(peer == alice_mac);
    std::uint32_t sent = 4;
    MHF_CHECK(alice_net.ptp_send(static_cast<int>(client), reinterpret_cast<const std::uint8_t *>("ping"), sent) == 0 && sent == 4);
    length = sizeof(buffer);
    MHF_CHECK(until([&] {
        length = sizeof(buffer);
        return bob_net.ptp_recv(static_cast<int>(accepted), buffer, length);
    }) == 0);
    MHF_CHECK(length == 4 && std::memcmp(buffer, "ping", 4) == 0);
    sent = 4;
    MHF_CHECK(bob_net.ptp_send(static_cast<int>(accepted), reinterpret_cast<const std::uint8_t *>("pong"), sent) == 0);
    length = sizeof(buffer);
    MHF_CHECK(until([&] {
        length = sizeof(buffer);
        return alice_net.ptp_recv(static_cast<int>(client), buffer, length);
    }) == 0);
    MHF_CHECK(length == 4 && std::memcmp(buffer, "pong", 4) == 0);
    MHF_CHECK(bob_net.ptp_stats().size() == 2); // the listener and the connection
    MHF_CHECK(alice_net.ptp_close(static_cast<int>(client)) == 0);
    length = sizeof(buffer);
    MHF_CHECK(until([&] {
        length = sizeof(buffer);
        return bob_net.ptp_recv(static_cast<int>(accepted), buffer, length);
    }) == sockerr::kDisconnected);
    MHF_CHECK(bob_net.ptp_close(static_cast<int>(accepted)) == 0 && bob_net.ptp_close(static_cast<int>(listener)) == 0);
    MHF_CHECK(alice_net.pdp_delete(static_cast<int>(a_pdp)) == 0 && alice_net.pdp_delete(static_cast<int>(a_pdp)) == sockerr::kInvalidSocketId);

    // ---- a private room (docs/14-rooms-and-friends.md section 3): Alice and Bob share a room code and meet in a hall;
    // Carol, in the public halls, does not see it, and joining the hall of the same number puts her somewhere else.
    {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count();
        const std::string code = "TEST" + std::to_string(1000 + now % 9000);
        const std::string hall = "MHP2Q0" + std::to_string(10 + now % 14);
        const HallPrefix prefix = {'M', 'H', 'P', '2', 'Q', '0'};
        MHF_CHECK(alice.library.disconnect() == 0 && bob.library.disconnect() == 0); // both left the group of the sockets
        (void)alice.wait([&] { return false; }, 1);
        (void)bob.wait([&] { return false; }, 1);
        for (Player *p : {&alice, &bob}) {
            p->library.set_room(code);
            p->library.add_hall_prefix(prefix);
        }
        MHF_CHECK(alice.library.room() == code);

        MHF_CHECK(alice.library.connect(group(hall.c_str())) == 0);
        MHF_CHECK(alice.wait([&] { return alice.library.state() == State::Connected; }));

        MHF_CHECK(bob.library.scan() == 0);
        MHF_CHECK(bob.wait([&] { return bob.library.state() == State::Disconnected && !bob.library.groups().empty(); }));
        MHF_CHECK(bob.library.groups().size() == 1 && bob.library.groups()[0].name == group(hall.c_str()));
        MHF_CHECK(bob.library.connect(group(hall.c_str())) == 0);
        MHF_CHECK(bob.wait([&] { return bob.library.state() == State::Connected && bob.library.peers().size() == 1; }));
        MHF_CHECK(!bob.library.peers().empty() && bob.library.peers()[0].nickname == "Alice");

        Player carol(server, "Carol", 3);
        MHF_CHECK(carol.wait([&] { return carol.library.online(); }));
        carol.library.add_hall_prefix(prefix);
        MHF_CHECK(carol.library.scan() == 0);
        MHF_CHECK(carol.wait([&] { return carol.library.state() == State::Disconnected && !carol.library.groups().empty(); }));
        bool sees_hall = false, sees_private = false;
        for (const ScanGroup &g : carol.library.groups()) {
            sees_hall = sees_hall || g.name == group(hall.c_str());
            sees_private = sees_private || g.name == private_name(code, group(hall.c_str()));
        }
        MHF_CHECK(!sees_hall && sees_private); // the server has the group, under a name her game never asks for
        MHF_CHECK(carol.library.connect(group(hall.c_str())) == 0);
        MHF_CHECK(carol.wait([&] { return carol.library.state() == State::Connected; }));
        MHF_CHECK(carol.wait([&] { return false; }, 2) || carol.library.peers().empty());
        MHF_CHECK(alice.wait([&] { return alice.library.peers().size() == 1; })); // Bob, and not Carol
        MHF_CHECK(!alice.library.peers().empty() && alice.library.peers()[0].nickname == "Bob");
        MHF_CHECK(carol.library.disconnect() == 0 && alice.library.disconnect() == 0 && bob.library.disconnect() == 0);
    }

    return mhf::test::finish("psp_adhocctl_server");
}
