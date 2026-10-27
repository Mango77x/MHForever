#pragma once

// How far away an ad hoc server is: the time it takes to open a TCP connection to it, the measure the Online settings
// show next to each server. Nothing is sent over the connection.

#include <optional>
#include <string>

namespace mhf::launcher {

// The port probed when `server` ("host" or "host:port") names none: the adhocctl port of PSP ad hoc servers.
inline constexpr unsigned kAdhocctlPort = 27312;

// Milliseconds to connect to `server`, or nothing when the name does not resolve, the server refuses, or it takes
// longer than `timeout_ms`. Blocks: call it off the UI thread.
std::optional<int> probe_server(const std::string &server, int timeout_ms);

} // namespace mhf::launcher
