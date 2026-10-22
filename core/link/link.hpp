#pragma once

// The local link between the game program and the launcher (docs/07-launcher-ux.md §3c): the launcher listens on the
// loopback interface, the game connects, and both exchange lines of UTF-8 text. A random token, passed to the game
// through its environment, keeps other programs of the machine from talking to either side.
//
// Console-independent: what the lines mean is up to the two programs.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace mhf::link {

inline constexpr std::size_t kMaxLine = 64 * 1024;

// One end of a connection. Sending is safe from any thread; reading is for one thread at a time.
class Connection {
public:
    ~Connection();
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    // Sends one line (without the newline; it must not contain one). False when the other end is gone or the line is
    // empty or longer than kMaxLine.
    bool send(const std::string &line);
    // Waits up to `timeout_ms` (0 = just look) for a whole line. Nothing when none arrived in time; once the other end
    // is gone, nothing for good (see open()).
    std::optional<std::string> receive(int timeout_ms = 0);
    // False once the other end closed the connection or it failed.
    [[nodiscard]] bool open() const { return open_; }

    struct Impl;
    explicit Connection(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> impl_;
    bool open_ = true;
};

// The launcher's end: listens on 127.0.0.1 on a port the system picks.
class Server {
public:
    ~Server();
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    // Starts listening. Nothing when no socket could be opened.
    static std::unique_ptr<Server> start();

    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] const std::string &token() const;
    // The value to give the game in its environment variable `kEnvironment`: "<port>:<token>".
    [[nodiscard]] std::string address() const;

    // Waits up to `timeout_ms` for the game to connect and prove it knows the token. Connections that do not (or that
    // say nothing within a second) are dropped and waiting goes on.
    std::unique_ptr<Connection> accept(int timeout_ms);

    struct Impl;

private:
    explicit Server(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// The game's end: connects to the address from `kEnvironment` ("<port>:<token>").
std::unique_ptr<Connection> connect(const std::string &address);

// Name of the environment variable that carries the address from the launcher to the game.
inline constexpr const char *kEnvironment = "MHF_LINK";

} // namespace mhf::link
