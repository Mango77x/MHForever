#include "adhocctl.hpp"

#include <algorithm>

namespace mhf::psp::adhoc {

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
    link_->request_connect(group);
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
        case LinkEvent::Kind::Scan:
            if (state_ == State::Scanning) scan_in_progress_.push_back({e.group, e.mac});
            break;
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
