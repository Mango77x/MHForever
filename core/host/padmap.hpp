#pragma once

// Controller bindings for the desktop host: which button of a controller stands for which pad button. A source is an
// SDL gamepad name ("south", "left_shoulder", "dpad_up", "left_trigger"...), which describes the position of the button
// on a standard layout whatever the maker of the controller calls it. A binding is one source or several joined with '+'
// (any of them presses the button); an empty value means the pad button has no source at all. `stick` chooses the analog
// stick that drives the PSP stick: "left", "right" or empty for none. Stored in the launcher settings and handed to the
// game with --padmap (docs/07-launcher-ux.md section 5).

#include <array>
#include <string>
#include <string_view>

namespace mhf::host {

struct PadBindings {
    std::string south{"south"};
    std::string east{"east"};
    std::string west{"west"};
    std::string north{"north"};
    std::string left_shoulder{"left_shoulder+left_trigger"};
    std::string right_shoulder{"right_shoulder+right_trigger"};
    std::string start{"start"};
    std::string select{"back"};
    std::string up{"dpad_up"};
    std::string down{"dpad_down"};
    std::string left{"dpad_left"};
    std::string right{"dpad_right"};
    std::string stick{"left"};

    friend bool operator==(const PadBindings &, const PadBindings &) = default;
};

// The action names of PadBindings, in the order the launcher shows them; the last one is the stick.
inline constexpr std::array<std::string_view, 13> kPadActions = {
    "south", "east", "west", "north", "left_shoulder", "right_shoulder", "start", "select",
    "up",    "down", "left", "right", "stick",
};

// The sources a pad button can take, as the launcher offers them.
inline constexpr std::array<std::string_view, 17> kPadSources = {
    "south",        "east",          "west",      "north",     "left_shoulder", "right_shoulder",
    "left_trigger", "right_trigger", "back",      "start",     "guide",         "left_stick",
    "right_stick",  "dpad_up",       "dpad_down", "dpad_left", "dpad_right",
};

[[nodiscard]] std::string *pad_binding(PadBindings &pad, std::string_view action);
[[nodiscard]] const std::string *pad_binding(const PadBindings &pad, std::string_view action);

// "south=south,left_shoulder=left_shoulder+left_trigger,stick=right,...": an action that is absent keeps its default, an
// action with an empty value is unbound. Names and values are trimmed; unknown actions are ignored.
[[nodiscard]] PadBindings parse_padmap(std::string_view text);
[[nodiscard]] std::string format_padmap(const PadBindings &pad);

} // namespace mhf::host
