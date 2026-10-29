// The ad hoc control library as a state machine (no network, no guest): joining a group, the peers the server announces,
// scans, handlers and what happens when the server goes away. Plus the address parsing of the server link.

#include "adhoc_link_aemu.hpp"
#include "adhocctl.hpp"

#include "check.hpp"

#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

using namespace mhf::psp::adhoc;

// A server that says what the test tells it to.
class FakeLink final : public AdhocLink {
public:
    void request_connect(const GroupName &group) override { connects.push_back(group); }
    void request_disconnect() override { ++disconnects; }
    void request_scan() override { ++scans; }
    std::vector<LinkEvent> take_events() override {
        std::vector<LinkEvent> out;
        out.swap(queued);
        return out;
    }
    [[nodiscard]] bool online() const override { return up; }

    LinkEvent &say(LinkEvent::Kind kind) {
        queued.emplace_back();
        queued.back().kind = kind;
        return queued.back();
    }

    std::vector<GroupName> connects;
    int disconnects = 0, scans = 0;
    bool up = true;
    std::vector<LinkEvent> queued;
};

GroupName group(const char *name) {
    GroupName g{};
    std::memcpy(g.data(), name, std::min<std::size_t>(std::strlen(name), g.size()));
    return g;
}

Mac mac(std::uint8_t last) { return Mac{0x02, 0, 0, 0, 0, last}; }

} // namespace

int main() {
    // Nothing works before sceNetAdhocctlInit; Init works once.
    {
        FakeLink link;
        Adhocctl c(&link);
        MHF_CHECK(c.connect(group("room")) == error::kNotInitialized);
        MHF_CHECK(c.scan() == error::kNotInitialized);
        MHF_CHECK(c.add_handler(0x08800000u, 1) == static_cast<std::int64_t>(error::kNotInitialized));
        MHF_CHECK(c.init() == 0);
        MHF_CHECK(c.init() == error::kAlreadyInitialized);
        MHF_CHECK(c.state() == State::Disconnected);
        MHF_CHECK(c.term() == 0 && !c.initialized());
    }

    // Handlers: ids, the limit, removal.
    {
        FakeLink link;
        Adhocctl c(&link);
        MHF_CHECK(c.init() == 0);
        for (int i = 0; i < 4; ++i) MHF_CHECK(c.add_handler(0x08800000u + static_cast<std::uint32_t>(i), 7) == i);
        MHF_CHECK(c.add_handler(0x08900000u, 7) == static_cast<std::int64_t>(error::kTooManyHandlers));
        MHF_CHECK(c.del_handler(1) == 0);
        MHF_CHECK(c.del_handler(1) == error::kIdNotFound);
        MHF_CHECK(c.handlers().size() == 3);
        MHF_CHECK(c.add_handler(0x08900000u, 9) == 4); // ids are not reused
    }

    // Joining a group: peers come before the server says the join is done; then the handlers hear CONNECT.
    {
        FakeLink link;
        Adhocctl c(&link);
        MHF_CHECK(c.init() == 0);
        MHF_CHECK(c.connect(group("PSP_GRP")) == 0);
        MHF_CHECK(link.connects.size() == 1 && link.connects[0] == group("PSP_GRP"));
        MHF_CHECK(c.connect(group("other")) == error::kAlreadyConnected); // a join is under way
        MHF_CHECK(c.poll().empty() && c.state() == State::Disconnected);

        auto &first = link.say(LinkEvent::Kind::Connect);
        first.mac = mac(1);
        first.nickname = "Rathalos";
        first.id = 10;
        auto &second = link.say(LinkEvent::Kind::Connect);
        second.mac = mac(2);
        second.nickname = "Rathian";
        second.id = 11;
        link.say(LinkEvent::Kind::LeaderMac).mac = mac(1);
        const auto notes = c.poll();
        MHF_CHECK(notes.size() == 1 && notes[0].event == kEventConnect && notes[0].error == 0);
        MHF_CHECK(c.state() == State::Connected);
        MHF_CHECK(c.peers().size() == 2 && c.peers()[0].nickname == "Rathalos" && c.peers()[1].mac == mac(2));
        MHF_CHECK(c.leader() == mac(1));
        MHF_CHECK(c.connect(group("PSP_GRP")) == error::kAlreadyConnected);
        MHF_CHECK(c.scan() == error::kAlreadyConnected);

        // A peer that is already known is updated, not repeated; one that leaves is removed.
        auto &again = link.say(LinkEvent::Kind::Connect);
        again.mac = mac(2);
        again.nickname = "Rathian II";
        again.id = 11;
        link.say(LinkEvent::Kind::Disconnect).id = 10;
        MHF_CHECK(c.poll().empty());
        MHF_CHECK(c.peers().size() == 1 && c.peers()[0].nickname == "Rathian II");

        // Leaving: the handlers hear DISCONNECT on the next poll, and the group is empty.
        MHF_CHECK(c.disconnect() == 0);
        MHF_CHECK(link.disconnects == 1 && c.state() == State::Disconnected && c.peers().empty());
        const auto left = c.poll();
        MHF_CHECK(left.size() == 1 && left[0].event == kEventDisconnect);
    }

    // A scan lists the groups the server knows, once it says it is complete; the group list survives until the next scan.
    {
        FakeLink link;
        Adhocctl c(&link);
        MHF_CHECK(c.init() == 0);
        MHF_CHECK(c.scan() == 0 && link.scans == 1 && c.state() == State::Scanning);
        MHF_CHECK(c.scan() == error::kBusy);
        auto &g1 = link.say(LinkEvent::Kind::Scan);
        g1.group = group("AAA");
        g1.mac = mac(5);
        auto &g2 = link.say(LinkEvent::Kind::Scan);
        g2.group = group("BBB");
        g2.mac = mac(6);
        MHF_CHECK(c.poll().empty());
        link.say(LinkEvent::Kind::ScanComplete);
        const auto notes = c.poll();
        MHF_CHECK(notes.size() == 1 && notes[0].event == kEventScan);
        MHF_CHECK(c.state() == State::Disconnected && c.groups().size() == 2 && c.groups()[1].leader == mac(6));
        // Scan results that arrive when no scan is running are ignored.
        link.say(LinkEvent::Kind::Scan).group = group("CCC");
        MHF_CHECK(c.poll().empty() && c.groups().size() == 2);
    }

    // Losing the server: a group in progress ends with DISCONNECT, a scan with SCAN.
    {
        FakeLink link;
        Adhocctl c(&link);
        MHF_CHECK(c.init() == 0 && c.connect(group("PSP_GRP")) == 0);
        link.say(LinkEvent::Kind::LeaderMac).mac = mac(1);
        MHF_CHECK(c.poll().size() == 1 && c.state() == State::Connected);
        link.say(LinkEvent::Kind::Error);
        const auto notes = c.poll();
        MHF_CHECK(notes.size() == 1 && notes[0].event == kEventDisconnect && c.state() == State::Disconnected);
        MHF_CHECK(c.scan() == 0);
        link.say(LinkEvent::Kind::Error);
        const auto scan_notes = c.poll();
        MHF_CHECK(scan_notes.size() == 1 && scan_notes[0].event == kEventScan && c.state() == State::Disconnected);
    }

    // No server configured: the library works, a scan finds nothing at once, a join never completes.
    {
        Adhocctl c(nullptr);
        MHF_CHECK(c.init() == 0 && !c.online());
        MHF_CHECK(c.scan() == 0);
        const auto notes = c.poll();
        MHF_CHECK(notes.size() == 1 && notes[0].event == kEventScan && c.groups().empty());
        MHF_CHECK(c.connect(group("PSP_GRP")) == 0 && c.state() == State::Disconnected);
    }

    // The server address: "host" or "host:port".
    {
        std::string host;
        std::uint16_t port = 0;
        MHF_CHECK(AemuLink::split_address("socom.cc", host, port) && host == "socom.cc" && port == 27312);
        MHF_CHECK(AemuLink::split_address("192.168.1.5:27400", host, port) && host == "192.168.1.5" && port == 27400);
        MHF_CHECK(!AemuLink::split_address("", host, port));
        MHF_CHECK(!AemuLink::split_address("host:", host, port));
        MHF_CHECK(!AemuLink::split_address(":27312", host, port));
        MHF_CHECK(!AemuLink::split_address("host:abc", host, port));
        MHF_CHECK(!AemuLink::split_address("host:70000", host, port));
        MHF_CHECK(!AemuLink::split_address("host:0", host, port));
    }

    // Group names in logs: Freedom Unite's halls as they are, the Japanese release's Shift-JIS bytes in hex.
    {
        MHF_CHECK(printable(group("MHP2Q011")) == "MHP2Q011");
        MHF_CHECK(printable(group("")) == "");
        const GroupName shift_jis = {'\x8F', '\x57', '\x89', '\xEF', '\x8F', '\x8A', '0', '1'};
        MHF_CHECK(printable(shift_jis) == "\\x8FW\\x89\\xEF\\x8F\\x8A01");
    }

    return mhf::test::finish("psp_adhocctl");
}
