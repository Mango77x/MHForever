#pragma once

// A network that lives inside one process: rooms whose members are all in this program. Used by tests and to run
// several console instances side by side without a real network. Every message is delivered, in order.

#include "net/room.hpp"

#include <memory>
#include <string>

namespace mhf::net {

class LocalNetwork {
public:
    LocalNetwork();
    ~LocalNetwork();
    LocalNetwork(const LocalNetwork &) = delete;
    LocalNetwork &operator=(const LocalNetwork &) = delete;

    // Creates the room `code` with this member as its first member. AlreadyExists if the code is taken.
    std::unique_ptr<Room> create(const std::string &code, const RoomDescription &description, const std::string &name,
                                 JoinError *error = nullptr);
    // Joins the room `code`. NoSuchRoom, Incompatible (another game, region, build or mode) or Full on failure.
    std::unique_ptr<Room> join(const std::string &code, const RoomDescription &description, const std::string &name,
                               JoinError *error = nullptr);

    struct State; // shared with the rooms, which may outlive the network object

private:
    std::shared_ptr<State> state_;
};

} // namespace mhf::net
