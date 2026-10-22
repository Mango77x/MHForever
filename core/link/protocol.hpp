#pragma once

// The lines the game and the launcher say to each other over the local link (link.hpp). One line = one message, words
// separated by spaces. Kept in one place so that both programs agree and the format can be tested.
//
//   game -> launcher   menu <x> <y> <width> <height> <fullscreen>   the player wants the launcher's menu; where the
//                                                                   game window is (screen pixels)
//   launcher -> game   set music <0..100>      music volume
//                      set sfx <0..100>        sound effects volume
//                      resume                  the menu is closed: go on playing
//                      quit                    end the game

#include <optional>
#include <string>

namespace mhf::link::protocol {

struct WindowRect {
    int x{}, y{}, width{}, height{};
    bool fullscreen{};
};

[[nodiscard]] std::string format_menu(const WindowRect &window);
[[nodiscard]] std::optional<WindowRect> parse_menu(const std::string &line);

struct Command {
    enum class Kind { SetMusic, SetSfx, Resume, Quit } kind = Kind::Resume;
    int value{}; // SetMusic/SetSfx: 0..100
};

[[nodiscard]] std::string format_command(const Command &command);
// Nothing for lines that are not a valid command (values out of range are refused, not clamped).
[[nodiscard]] std::optional<Command> parse_command(const std::string &line);

} // namespace mhf::link::protocol
