#pragma once

// Rooms: the one service core/net offers to console adapters (docs/08-multiplayer.md §1.1).
//
// A room is a virtual LAN shared by up to `max_members` players of the same game build. Members send each other
// messages, either to one member or to everyone; what the messages mean (ad-hoc groups and ports on the PSP, local
// wireless on other consoles, controller inputs for netplay) is up to the adapter. Nothing here knows which console
// or game is running.
//
// Every Room is safe to use from several threads.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mhf::net {

// Packets: members exchange the game's own network traffic. Inputs: members exchange controller inputs per frame.
enum class Mode : std::uint8_t { Packets, Inputs };

// Unreliable: may be lost or arrive out of order (datagrams). Reliable: arrives once and in order with respect to the
// other reliable messages between the same two members (streams).
enum class Delivery : std::uint8_t { Unreliable, Reliable };

struct RoomDescription {
    std::string game_id; // e.g. "mhfu"
    std::string region;  // e.g. "ULES01213"
    std::string build;   // the build of the program, so that members run the same code
    Mode mode = Mode::Packets;
    int max_members = 4;

    // Members must run the same game, region, build and mode (the size limit is the host's to choose).
    [[nodiscard]] bool compatible_with(const RoomDescription &other) const {
        return game_id == other.game_id && region == other.region && build == other.build && mode == other.mode;
    }
};

// A member's id within one room: given in order of arrival, never reused while the room exists.
using MemberId = std::uint32_t;
inline constexpr MemberId kEveryone = 0xFFFFFFFFu;

struct Member {
    MemberId id{};
    std::string name;
};

struct Message {
    MemberId from{};
    Delivery delivery = Delivery::Unreliable;
    std::vector<std::uint8_t> data;
};

struct RoomEvent {
    enum class Kind : std::uint8_t { Joined, Left } kind = Kind::Joined;
    Member member;
};

enum class JoinError : std::uint8_t { None, NoSuchRoom, Incompatible, Full, AlreadyExists };

class Room {
public:
    // Largest message a transport must carry (bigger ones are refused by send).
    static constexpr std::size_t kMaxMessage = 64 * 1024;

    virtual ~Room() = default;

    [[nodiscard]] virtual MemberId self() const = 0;
    [[nodiscard]] virtual RoomDescription description() const = 0;
    // Everyone in the room, this member included, in order of arrival. Empty once left.
    [[nodiscard]] virtual std::vector<Member> members() const = 0;

    // Sends to one member or to everyone else (kEveryone). False when the message is too big, the member is not in
    // the room, or this member has left.
    virtual bool send(MemberId to, Delivery delivery, std::span<const std::uint8_t> data) = 0;
    // The next message for this member, if any (never blocks).
    virtual std::optional<Message> receive() = 0;
    // The next change in membership, if any (never blocks). This member's own arrival is not reported.
    virtual std::optional<RoomEvent> next_event() = 0;

    // Leaves the room; the others see this member leave. Also done by the destructor.
    virtual void leave() = 0;
};

} // namespace mhf::net
