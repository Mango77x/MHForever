#include "adhocctl.hpp"

#include <algorithm>
#include <cctype>

namespace mhf::psp::adhoc {

namespace {

constexpr const char *kBase32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // Crockford

HallPrefix prefix_of(const GroupName &name) {
    HallPrefix prefix{};
    std::copy_n(name.begin(), prefix.size(), prefix.begin());
    return prefix;
}

} // namespace

std::string normalize_room_code(std::string_view text) {
    std::string code;
    for (const char raw : text) {
        if (raw == '-' || raw == ' ') continue;
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        if (std::string_view(kBase32).find(c) == std::string_view::npos) return {};
        code.push_back(c);
    }
    return code.size() == 8 ? code : std::string();
}

GroupName private_name(const std::string &code, const GroupName &game_name) {
    // FNV-1a, as for the virtual MAC (hle_net.cpp): the name only has to differ between rooms and halls, and 25 bits
    // cannot give the code back.
    std::uint64_t h = 0xCBF29CE484222325ull;
    const auto mix = [&h](char c) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x100000001B3ull;
    };
    for (const char c : std::string_view("mhf-room", 9)) mix(c); // with its terminating zero, as a separator
    for (const char c : code) mix(c);
    for (std::size_t i = 0; i < 6; ++i) mix(game_name[i]);
    GroupName name{};
    name[0] = 'F';
    for (std::size_t i = 1; i < 6; ++i) name[i] = kBase32[(h >> (5u * (i - 1u))) & 31u];
    name[6] = game_name[6];
    name[7] = game_name[7];
    return name;
}

void Adhocctl::add_hall_prefix(const HallPrefix &prefix) {
    if (std::find(prefixes_.begin(), prefixes_.end(), prefix) == prefixes_.end()) prefixes_.push_back(prefix);
}

std::uint32_t Adhocctl::init() {
    if (initialized_) return error::kAlreadyInitialized;
    initialized_ = true;
    state_ = State::Disconnected;
    peers_.clear();
    groups_.clear();
    scan_in_progress_.clear();
    connecting_ = false;
    pending_.clear();
    return 0;
}

bool Adhocctl::set_link(AdhocLink *link) {
    if (!idle()) return false;
    link_ = link;
    peers_.clear();
    groups_.clear();
    scan_in_progress_.clear();
    return true;
}

std::uint32_t Adhocctl::term() {
    if (!initialized_) return 0;
    if (link_ != nullptr && (state_ == State::Connected || connecting_)) link_->request_disconnect();
    initialized_ = false;
    state_ = State::Disconnected;
    handlers_.clear();
    peers_.clear();
    groups_.clear();
    scan_in_progress_.clear();
    connecting_ = false;
    pending_.clear();
    return 0;
}

std::int64_t Adhocctl::add_handler(std::uint32_t function, std::uint32_t argument) {
    if (!initialized_) return error::kNotInitialized;
    if (handlers_.size() >= kMaxHandlers) return error::kTooManyHandlers;
    Handler handler;
    handler.id = next_handler_id_++;
    handler.function = function;
    handler.argument = argument;
    handlers_.push_back(handler);
    return handler.id;
}

std::uint32_t Adhocctl::del_handler(int id) {
    if (!initialized_) return error::kNotInitialized;
    const auto it = std::find_if(handlers_.begin(), handlers_.end(), [id](const Handler &h) { return h.id == id; });
    if (it == handlers_.end()) return error::kIdNotFound;
    handlers_.erase(it);
    return 0;
}

std::uint32_t Adhocctl::connect(const GroupName &group) {
    if (!initialized_) return error::kNotInitialized;
    if (state_ == State::Connected || connecting_) return error::kAlreadyConnected;
    if (state_ == State::Scanning) return error::kBusy;
    if (link_ == nullptr) return 0; // nothing will ever answer
    peers_.clear();
    connecting_ = true;
    joined_room_ = room_;
    add_hall_prefix(prefix_of(group));
    link_->request_connect(room_.empty() ? group : private_name(room_, group));
    return 0;
}

std::uint32_t Adhocctl::disconnect() {
    if (!initialized_) return error::kNotInitialized;
    if (link_ != nullptr && (state_ == State::Connected || connecting_)) link_->request_disconnect();
    const bool was_connected = state_ == State::Connected;
    state_ = State::Disconnected;
    connecting_ = false;
    peers_.clear();
    // The game's handlers hear of it as they would from the PSP: later, from the library's own thread.
    if (was_connected) pending_.push_back({kEventDisconnect, 0});
    return 0;
}

std::uint32_t Adhocctl::scan() {
    if (!initialized_) return error::kNotInitialized;
    if (state_ == State::Connected || connecting_) return error::kAlreadyConnected;
    if (state_ == State::Scanning) return error::kBusy;
    state_ = State::Scanning;
    scan_in_progress_.clear();
    if (link_ == nullptr) {
        // No server: the scan finds nothing.
        pending_.push_back({kEventScan, 0});
        state_ = State::Disconnected;
        return 0;
    }
    link_->request_scan();
    return 0;
}

std::vector<Notification> Adhocctl::poll() {
    std::vector<Notification> out;
    out.swap(pending_);
    if (!initialized_ || link_ == nullptr) return out;
    for (const LinkEvent &e : link_->take_events()) {
        switch (e.kind) {
        case LinkEvent::Kind::Scan: {
            if (state_ != State::Scanning) break;
            if (room_.empty()) {
                scan_in_progress_.push_back({e.group, e.mac});
                break;
            }
            // In a private room the game sees only that room's halls, under its own names; every other group is left out.
            for (const HallPrefix &prefix : prefixes_) {
                GroupName game_name{};
                std::copy(prefix.begin(), prefix.end(), game_name.begin());
                game_name[6] = e.group[6];
                game_name[7] = e.group[7];
                if (private_name(room_, game_name) == e.group) {
                    scan_in_progress_.push_back({game_name, e.mac});
                    break;
                }
            }
            break;
        }
        case LinkEvent::Kind::ScanComplete:
            if (state_ == State::Scanning) {
                groups_ = scan_in_progress_;
                state_ = State::Disconnected;
                out.push_back({kEventScan, 0});
            }
            break;
        case LinkEvent::Kind::Connect: {
            // A peer of our group (the server says it for every member already there, and for each one that joins).
            if (!connecting_ && state_ != State::Connected) break;
            const auto it = std::find_if(peers_.begin(), peers_.end(), [&](const Peer &p) { return p.mac == e.mac; });
            if (it == peers_.end()) peers_.push_back({e.mac, e.nickname, e.id});
            else *it = {e.mac, e.nickname, e.id};
            break;
        }
        case LinkEvent::Kind::LeaderMac:
            if (connecting_) {
                leader_ = e.mac;
                connecting_ = false;
                state_ = State::Connected;
                out.push_back({kEventConnect, 0});
            }
            break;
        case LinkEvent::Kind::Disconnect:
            peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [&](const Peer &p) { return p.id == e.id; }), peers_.end());
            break;
        case LinkEvent::Kind::Chat: break;
        case LinkEvent::Kind::Error:
            // The server is gone. A group we were in is gone with it; a scan under way ends without results.
            if (state_ == State::Connected || connecting_) out.push_back({kEventDisconnect, 0});
            else if (state_ == State::Scanning) out.push_back({kEventScan, 0});
            state_ = State::Disconnected;
            connecting_ = false;
            peers_.clear();
            scan_in_progress_.clear();
            break;
        }
    }
    return out;
}

} // namespace mhf::psp::adhoc
