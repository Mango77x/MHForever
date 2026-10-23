#pragma once

// AdhocLink over the aemu_postoffice client: logs in to a PSP ad hoc server (PPSSPP's and aemu's protocol, TCP 27312) on a
// thread of its own, so connecting never holds the game up, and keeps the connection alive. The aemu session is used by
// that thread only. docs/08-multiplayer.md section 1.2.

#include "adhocctl.hpp"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mhf::psp::adhoc {

class AemuLink final : public AdhocLink {
public:
    struct Config {
        std::string host;            // "name" or "name:port"; port 27312 when absent
        std::string product_code;    // ULES01213: the server groups players by game
        std::string nickname;
        Mac mac{};
    };

    explicit AemuLink(Config config);
    ~AemuLink() override;
    AemuLink(const AemuLink &) = delete;
    AemuLink &operator=(const AemuLink &) = delete;

    void request_connect(const GroupName &group) override;
    void request_disconnect() override;
    void request_scan() override;
    std::vector<LinkEvent> take_events() override;
    [[nodiscard]] bool online() const override { return online_; }

    // "host" or "host:port" split into its parts. False for an empty or malformed address.
    static bool split_address(const std::string &address, std::string &host, std::uint16_t &port);

private:
    struct Command {
        enum class Kind { Connect, Disconnect, Scan } kind = Kind::Scan;
        GroupName group{};
    };
    void run();
    bool open_session(void *&session);

    Config config_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> online_{false};
    std::mutex mutex_;
    std::deque<Command> commands_;
    std::vector<LinkEvent> events_;
};

} // namespace mhf::psp::adhoc
