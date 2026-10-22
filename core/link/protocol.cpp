#include "link/protocol.hpp"

#include <charconv>
#include <sstream>
#include <vector>

namespace mhf::link::protocol {

namespace {

std::vector<std::string> words(const std::string &line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

std::optional<int> number(const std::string &text) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) return std::nullopt;
    return value;
}

} // namespace

std::string format_menu(const WindowRect &w) {
    return "menu " + std::to_string(w.x) + " " + std::to_string(w.y) + " " + std::to_string(w.width) + " " +
           std::to_string(w.height) + " " + (w.fullscreen ? "1" : "0");
}

std::optional<WindowRect> parse_menu(const std::string &line) {
    const auto w = words(line);
    if (w.size() != 6 || w[0] != "menu") return std::nullopt;
    const auto x = number(w[1]), y = number(w[2]), width = number(w[3]), height = number(w[4]), full = number(w[5]);
    if (!x || !y || !width || !height || !full || *width <= 0 || *height <= 0 || (*full != 0 && *full != 1)) {
        return std::nullopt;
    }
    return WindowRect{*x, *y, *width, *height, *full == 1};
}

std::string format_command(const Command &c) {
    switch (c.kind) {
    case Command::Kind::SetMusic: return "set music " + std::to_string(c.value);
    case Command::Kind::SetSfx: return "set sfx " + std::to_string(c.value);
    case Command::Kind::Resume: return "resume";
    case Command::Kind::Quit: return "quit";
    }
    return "resume";
}

std::optional<Command> parse_command(const std::string &line) {
    const auto w = words(line);
    if (w.size() == 1 && w[0] == "resume") return Command{Command::Kind::Resume, 0};
    if (w.size() == 1 && w[0] == "quit") return Command{Command::Kind::Quit, 0};
    if (w.size() != 3 || w[0] != "set") return std::nullopt;
    const auto value = number(w[2]);
    if (!value) return std::nullopt;
    if (w[1] == "music" || w[1] == "sfx") {
        if (*value < 0 || *value > 100) return std::nullopt;
        return Command{w[1] == "music" ? Command::Kind::SetMusic : Command::Kind::SetSfx, *value};
    }
    return std::nullopt;
}

} // namespace mhf::link::protocol
