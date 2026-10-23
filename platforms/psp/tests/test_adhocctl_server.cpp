// The ad hoc control library against a real PSP ad hoc server (aemu_postoffice's reference server, or any other): two
// players log in, one creates a group, the other scans, finds it and joins; they see each other; one leaves.
//
// Needs a server, so it only runs when MHF_ADHOC_TEST_SERVER is set ("host" or "host:port"); otherwise it passes at once.
// Not part of the game data tests: it uses made-up product codes and nicknames.

#include "adhoc_link_aemu.hpp"
#include "adhocctl.hpp"

#include "check.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

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

    Player alice(server, "Alice", 1);
    Player bob(server, "Bob", 2);
    MHF_CHECK(alice.wait([&] { return alice.library.online(); }));
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

    // Bob leaves; Alice notices.
    MHF_CHECK(bob.library.disconnect() == 0);
    MHF_CHECK(alice.wait([&] { return alice.library.peers().empty(); }));
    MHF_CHECK(alice.library.state() == State::Connected);

    return mhf::test::finish("psp_adhocctl_server");
}
