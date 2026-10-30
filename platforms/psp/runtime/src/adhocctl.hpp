#pragma once

// The PSP's ad hoc control library (sceNetAdhocctl) as a state machine on the host, independent of the guest and of the
// network: it keeps the state, the peers of the group and the groups found by a scan, and turns what the ad hoc server
// says (AdhocLink) into the events the game's handlers expect. docs/08-multiplayer.md section 1.2.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mhf::psp::adhoc {

using Mac = std::array<std::uint8_t, 6>;
using GroupName = std::array<char, 8>; // not necessarily NUL-terminated, as on the PSP

// A group name for logs: printable ASCII as it is, any other byte as \xHH (the Japanese release's names are Shift-JIS).
inline std::string printable(const GroupName &name) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (const char c : name) {
        const auto b = static_cast<unsigned char>(c);
        if (b == 0) break;
        if (b >= 0x20 && b < 0x7F && b != '\\') {
            out.push_back(c);
        } else {
            out += "\\x";
            out.push_back(hex[b >> 4]);
            out.push_back(hex[b & 15]);
        }
    }
    return out;
}

// What the server (or a test) reports, in the order it happened.
struct LinkEvent {
    enum class Kind { Scan, ScanComplete, Connect, LeaderMac, Disconnect, Chat, Error } kind = Kind::Error;
    GroupName group{};       // Scan
    Mac mac{};               // Scan: the group's leader; Connect: the peer; LeaderMac: our group's leader
    std::uint32_t id{};      // Connect, Disconnect: the server's identifier for the peer
    std::string nickname;    // Connect
};

// The connection to an ad hoc server, from the point of view of the control library. Calls never block.
class AdhocLink {
public:
    virtual ~AdhocLink() = default;
    virtual void request_connect(const GroupName &group) = 0;
    virtual void request_disconnect() = 0;
    virtual void request_scan() = 0;
    // Everything that arrived since the last call.
    virtual std::vector<LinkEvent> take_events() = 0;
    // Logged in to the server (false while connecting, and after the server was lost).
    [[nodiscard]] virtual bool online() const = 0;
};

// sceNetAdhocctl error codes the library returns.
namespace error {
inline constexpr std::uint32_t kAlreadyConnected = 0x80410B02u;
inline constexpr std::uint32_t kInvalidArg = 0x80410B04u;
inline constexpr std::uint32_t kIdNotFound = 0x80410B06u;
inline constexpr std::uint32_t kAlreadyInitialized = 0x80410B07u;
inline constexpr std::uint32_t kNotInitialized = 0x80410B08u;
inline constexpr std::uint32_t kBusy = 0x80410B10u;
inline constexpr std::uint32_t kTooManyHandlers = 0x80410B12u;
} // namespace error

// Private rooms (docs/14-rooms-and-friends.md section 3): a room code renames the groups a game asks for, so only players
// with the same code meet, on any public server. A game name of 8 bytes keeps its last two (the hall number) and gets
// the first six from the code and its own first six: "F" and five base32 characters.
using HallPrefix = std::array<char, 6>;
// "k7qf-29xm" -> "K7QF29XM" (Crockford base32: O reads as 0, I and L as 1); "" when it is not 8 such characters.
std::string normalize_room_code(std::string_view text);
// The name the server sees for `game_name` in room `code` (a normalized code).
GroupName private_name(const std::string &code, const GroupName &game_name);

enum class State : std::int32_t { Disconnected = 0, Connected = 1, Scanning = 2 };

// What a handler is called with: (event, error, argument).
enum Event : std::int32_t { kEventError = 0, kEventConnect = 1, kEventDisconnect = 2, kEventScan = 3 };

struct Peer {
    Mac mac{};
    std::string nickname;
    std::uint32_t id{};
};

struct ScanGroup {
    GroupName name{};
    Mac leader{};
};

struct Handler {
    int id{};
    std::uint32_t function{};
    std::uint32_t argument{};
};

struct Notification {
    std::int32_t event{};
    std::int32_t error{};
};

class Adhocctl {
public:
    static constexpr std::size_t kMaxHandlers = 4;

    // `link` stays with the caller; nullptr means there is no server (the library works, nothing ever connects).
    explicit Adhocctl(AdhocLink *link) : link_(link) {}

    [[nodiscard]] std::uint32_t init();
    [[nodiscard]] std::uint32_t term();
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    // Handlers: the id of the new one, or an error code (>= 0x80000000).
    [[nodiscard]] std::int64_t add_handler(std::uint32_t function, std::uint32_t argument);
    [[nodiscard]] std::uint32_t del_handler(int id);
    [[nodiscard]] const std::vector<Handler> &handlers() const noexcept { return handlers_; }

    // Joins (or creates, when it does not exist yet) the group. The result comes as an event once the server answered.
    [[nodiscard]] std::uint32_t connect(const GroupName &group);
    [[nodiscard]] std::uint32_t disconnect();
    [[nodiscard]] std::uint32_t scan();

    // Takes in what the link reported. Returns the events the handlers are to be called with, in order.
    [[nodiscard]] std::vector<Notification> poll();

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] const std::vector<Peer> &peers() const noexcept { return peers_; }
    [[nodiscard]] const std::vector<ScanGroup> &groups() const noexcept { return groups_; }
    [[nodiscard]] const Mac &leader() const noexcept { return leader_; }
    [[nodiscard]] bool online() const { return link_ != nullptr && link_->online(); }
    // In no group, joining none and not scanning: the moment another server can take over.
    [[nodiscard]] bool idle() const noexcept { return state_ == State::Disconnected && !connecting_; }
    // Another link (another server), only while idle(); false otherwise. What the old one found is forgotten, the
    // handlers stay. The old link may go once this returned true.
    bool set_link(AdhocLink *link);

    // The private room the next scan and the next join use; "" (or a code that is not valid) is the public halls. It can
    // change at any time: a group already joined stays as it is until the game leaves it.
    void set_room(std::string_view code) { room_ = normalize_room_code(code); }
    [[nodiscard]] const std::string &room() const noexcept { return room_; }
    // The room of the group joined last ("" the public halls): the one the game is in while state() is Connected.
    [[nodiscard]] const std::string &joined_room() const noexcept { return joined_room_; }
    // The first six bytes of the game's hall names, so that a scan in a private room can report the halls under the
    // names the game knows. A prefix the game joins with is learnt too.
    void add_hall_prefix(const HallPrefix &prefix);

private:
    AdhocLink *link_;
    std::string room_, joined_room_;
    std::vector<HallPrefix> prefixes_;
    bool initialized_ = false;
    State state_ = State::Disconnected;
    std::vector<Handler> handlers_;
    int next_handler_id_ = 0;
    std::vector<Peer> peers_;
    std::vector<ScanGroup> groups_;
    std::vector<ScanGroup> scan_in_progress_;
    Mac leader_{};
    bool connecting_ = false;
    std::vector<Notification> pending_; // events raised by calls, delivered by the next poll
};

} // namespace mhf::psp::adhoc
