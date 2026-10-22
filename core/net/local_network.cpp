#include "net/local_network.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>

namespace mhf::net {

namespace {

struct Inbox {
    std::deque<Message> messages;
    std::deque<RoomEvent> events;
};

struct RoomState {
    RoomDescription description;
    MemberId next_id = 1;
    std::vector<Member> members;        // in order of arrival
    std::map<MemberId, Inbox> inboxes;  // one per member
};

} // namespace

struct LocalNetwork::State {
    std::mutex mutex;
    std::map<std::string, RoomState> rooms;
};

namespace {

class LocalRoom final : public Room {
public:
    LocalRoom(std::shared_ptr<LocalNetwork::State> state, std::string code, MemberId self)
        : state_(std::move(state)), code_(std::move(code)), self_(self) {}
    ~LocalRoom() override { leave(); }

    MemberId self() const override { return self_; }

    RoomDescription description() const override {
        std::lock_guard lock(state_->mutex);
        const RoomState *room = find();
        return room != nullptr ? room->description : RoomDescription{};
    }

    std::vector<Member> members() const override {
        std::lock_guard lock(state_->mutex);
        const RoomState *room = left_ ? nullptr : find();
        return room != nullptr ? room->members : std::vector<Member>{};
    }

    bool send(MemberId to, Delivery delivery, std::span<const std::uint8_t> data) override {
        if (data.size() > kMaxMessage) return false;
        std::lock_guard lock(state_->mutex);
        RoomState *room = left_ ? nullptr : find();
        if (room == nullptr) return false;
        Message message{self_, delivery, std::vector<std::uint8_t>(data.begin(), data.end())};
        if (to == kEveryone) {
            for (auto &[id, inbox] : room->inboxes) {
                if (id != self_) inbox.messages.push_back(message);
            }
            return true;
        }
        const auto it = room->inboxes.find(to);
        if (it == room->inboxes.end() || to == self_) return false;
        it->second.messages.push_back(std::move(message));
        return true;
    }

    std::optional<Message> receive() override {
        std::lock_guard lock(state_->mutex);
        Inbox *inbox = mine();
        if (inbox == nullptr || inbox->messages.empty()) return std::nullopt;
        Message message = std::move(inbox->messages.front());
        inbox->messages.pop_front();
        return message;
    }

    std::optional<RoomEvent> next_event() override {
        std::lock_guard lock(state_->mutex);
        Inbox *inbox = mine();
        if (inbox == nullptr || inbox->events.empty()) return std::nullopt;
        RoomEvent event = std::move(inbox->events.front());
        inbox->events.pop_front();
        return event;
    }

    void leave() override {
        std::lock_guard lock(state_->mutex);
        if (left_) return;
        left_ = true;
        const auto it = state_->rooms.find(code_);
        if (it == state_->rooms.end()) return;
        RoomState &room = it->second;
        const auto member = std::find_if(room.members.begin(), room.members.end(),
                                         [&](const Member &m) { return m.id == self_; });
        if (member == room.members.end()) return;
        const Member gone = *member;
        room.members.erase(member);
        room.inboxes.erase(self_);
        for (auto &[id, inbox] : room.inboxes) inbox.events.push_back({RoomEvent::Kind::Left, gone});
        if (room.members.empty()) state_->rooms.erase(it); // the code is free again
    }

private:
    const RoomState *find() const {
        const auto it = state_->rooms.find(code_);
        return it != state_->rooms.end() ? &it->second : nullptr;
    }
    RoomState *find() {
        const auto it = state_->rooms.find(code_);
        return it != state_->rooms.end() ? &it->second : nullptr;
    }
    Inbox *mine() {
        RoomState *room = left_ ? nullptr : find();
        if (room == nullptr) return nullptr;
        const auto it = room->inboxes.find(self_);
        return it != room->inboxes.end() ? &it->second : nullptr;
    }

    std::shared_ptr<LocalNetwork::State> state_;
    std::string code_;
    MemberId self_;
    bool left_ = false; // guarded by state_->mutex
};

// Adds a member to `room` (the caller holds the mutex) and tells the others.
MemberId add_member(RoomState &room, const std::string &name) {
    const Member member{room.next_id++, name};
    for (auto &[id, inbox] : room.inboxes) inbox.events.push_back({RoomEvent::Kind::Joined, member});
    room.members.push_back(member);
    room.inboxes[member.id];
    return member.id;
}

} // namespace

LocalNetwork::LocalNetwork() : state_(std::make_shared<State>()) {}
LocalNetwork::~LocalNetwork() = default;

std::unique_ptr<Room> LocalNetwork::create(const std::string &code, const RoomDescription &description,
                                           const std::string &name, JoinError *error) {
    std::lock_guard lock(state_->mutex);
    if (error != nullptr) *error = JoinError::None;
    if (state_->rooms.contains(code)) {
        if (error != nullptr) *error = JoinError::AlreadyExists;
        return nullptr;
    }
    RoomState &room = state_->rooms[code];
    room.description = description;
    room.description.max_members = std::max(description.max_members, 1);
    return std::make_unique<LocalRoom>(state_, code, add_member(room, name));
}

std::unique_ptr<Room> LocalNetwork::join(const std::string &code, const RoomDescription &description,
                                         const std::string &name, JoinError *error) {
    std::lock_guard lock(state_->mutex);
    if (error != nullptr) *error = JoinError::None;
    const auto it = state_->rooms.find(code);
    JoinError why = JoinError::None;
    if (it == state_->rooms.end()) why = JoinError::NoSuchRoom;
    else if (!it->second.description.compatible_with(description)) why = JoinError::Incompatible;
    else if (static_cast<int>(it->second.members.size()) >= it->second.description.max_members) why = JoinError::Full;
    if (why != JoinError::None) {
        if (error != nullptr) *error = why;
        return nullptr;
    }
    return std::make_unique<LocalRoom>(state_, code, add_member(it->second, name));
}

} // namespace mhf::net
