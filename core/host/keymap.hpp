#pragma once

// Keyboard bindings for the desktop host: which physical key stands for which pad button. Values are UI Events
// "code" strings ("KeyK", "ArrowLeft", "Space"...): they name the key's position, not the character printed on it, so
// WASD stays where a player expects it on AZERTY or QWERTZ. Stored in the launcher's settings and handed to the game
// with --keymap. An empty value means the action has no key at all (docs/07-launcher-ux.md section 5).

#include <array>
#include <string>
#include <string_view>

namespace mhf::host {

struct KeyBindings {
    std::string south{"KeyK"};
    std::string east{"KeyL"};
    std::string west{"KeyJ"};
    std::string north{"KeyI"};
    std::string left_shoulder{"KeyQ"};
    std::string right_shoulder{"KeyE"};
    std::string start{"Enter"};
    std::string select{"Backspace"};
    std::string up{"ArrowUp"};
    std::string down{"ArrowDown"};
    std::string left{"ArrowLeft"};
    std::string right{"ArrowRight"};
    std::string stick_up{"KeyW"};
    std::string stick_down{"KeyS"};
    std::string stick_left{"KeyA"};
    std::string stick_right{"KeyD"};

    friend bool operator==(const KeyBindings &, const KeyBindings &) = default;
};

// The action names of KeyBindings, in the order the launcher shows them.
inline constexpr std::array<std::string_view, 16> kKeyActions = {
    "south",  "east",  "west",  "north", "left_shoulder", "right_shoulder", "start", "select",
    "up",     "down",  "left",  "right", "stick_up",      "stick_down",     "stick_left", "stick_right",
};

// The binding of `action`, or nullptr when there is no such action.
[[nodiscard]] std::string *key_binding(KeyBindings &keys, std::string_view action);
[[nodiscard]] const std::string *key_binding(const KeyBindings &keys, std::string_view action);

// "south=KeyK,east=KeyL,...": an action that is absent keeps its default, an action with an empty value is unbound.
// Names and values are trimmed; unknown actions are ignored.
[[nodiscard]] KeyBindings parse_keymap(std::string_view text);
[[nodiscard]] std::string format_keymap(const KeyBindings &keys);

} // namespace mhf::host
