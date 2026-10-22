#include "net/local_network.hpp"

#include "check.hpp"

#include <cstdint>
#include <vector>

namespace {

using namespace mhf::net;

RoomDescription fu() { return {"mhfu", "ULES01213", "0.2.3", Mode::Packets, 4}; }

std::vector<std::uint8_t> bytes(std::initializer_list<std::uint8_t> list) { return list; }

} // namespace

int main() {
    LocalNetwork network;

    // Creating and joining; members are listed in order of arrival with ids that grow.
    JoinError error{};
    auto host = network.create("ABC123", fu(), "host", &error);
    MHF_CHECK(host != nullptr && error == JoinError::None);
    MHF_CHECK(network.create("ABC123", fu(), "again", &error) == nullptr && error == JoinError::AlreadyExists);
    auto guest = network.join("ABC123", fu(), "guest", &error);
    MHF_CHECK(guest != nullptr && error == JoinError::None);
    MHF_CHECK(guest->members().size() == 2 && guest->members()[0].name == "host");
    MHF_CHECK(guest->self() != host->self());

    // The host hears the guest arrive; the guest does not hear itself.
    const auto joined = host->next_event();
    MHF_CHECK(joined && joined->kind == RoomEvent::Kind::Joined && joined->member.id == guest->self());
    MHF_CHECK(!guest->next_event());

    // Refusals: unknown code, another build, a full room.
    MHF_CHECK(network.join("NOPE00", fu(), "x", &error) == nullptr && error == JoinError::NoSuchRoom);
    RoomDescription other = fu();
    other.build = "0.2.2";
    MHF_CHECK(network.join("ABC123", other, "x", &error) == nullptr && error == JoinError::Incompatible);
    other = fu();
    other.mode = Mode::Inputs;
    MHF_CHECK(network.join("ABC123", other, "x", &error) == nullptr && error == JoinError::Incompatible);
    auto third = network.join("ABC123", fu(), "third", &error);
    auto fourth = network.join("ABC123", fu(), "fourth", &error);
    MHF_CHECK(third && fourth);
    MHF_CHECK(network.join("ABC123", fu(), "fifth", &error) == nullptr && error == JoinError::Full);

    // One member, in order; everyone else; never back to the sender.
    MHF_CHECK(host->send(guest->self(), Delivery::Reliable, bytes({1, 2})));
    MHF_CHECK(host->send(guest->self(), Delivery::Unreliable, bytes({3})));
    auto m = guest->receive();
    MHF_CHECK(m && m->from == host->self() && m->delivery == Delivery::Reliable && m->data == bytes({1, 2}));
    m = guest->receive();
    MHF_CHECK(m && m->data == bytes({3}));
    MHF_CHECK(!guest->receive());
    MHF_CHECK(guest->send(kEveryone, Delivery::Unreliable, bytes({9})));
    MHF_CHECK(host->receive() && third->receive() && fourth->receive());
    MHF_CHECK(!guest->receive());
    MHF_CHECK(!host->send(host->self(), Delivery::Unreliable, bytes({0})));
    MHF_CHECK(!host->send(12345, Delivery::Unreliable, bytes({0})));
    std::vector<std::uint8_t> huge(Room::kMaxMessage + 1);
    MHF_CHECK(!host->send(guest->self(), Delivery::Unreliable, huge));

    // Leaving: the others hear it, the room has a free place again, and the one who left can do nothing more.
    while (host->next_event()) {
    }
    const MemberId gone = fourth->self();
    fourth->leave();
    const auto left = host->next_event();
    MHF_CHECK(left && left->kind == RoomEvent::Kind::Left && left->member.id == gone);
    MHF_CHECK(fourth->members().empty());
    MHF_CHECK(!fourth->send(kEveryone, Delivery::Unreliable, bytes({1})));
    MHF_CHECK(!host->send(gone, Delivery::Unreliable, bytes({1})));
    auto fifth = network.join("ABC123", fu(), "fifth", &error);
    MHF_CHECK(fifth != nullptr && fifth->self() != gone); // ids are never reused

    // When the last member leaves, the code is free again (destructors leave too).
    host.reset();
    guest.reset();
    third.reset();
    fifth.reset();
    MHF_CHECK(network.join("ABC123", fu(), "late", &error) == nullptr && error == JoinError::NoSuchRoom);
    MHF_CHECK(network.create("ABC123", fu(), "new host") != nullptr);

    return mhf::test::finish("test_local_network");
}
