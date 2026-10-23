// Keyboard bindings: the names the launcher stores, and the scancodes the desktop host reads (no game data needed).

#include "host/keymap.hpp"
#include "host/keymap_sdl.hpp"

#include "check.hpp"

#include <string>
#include <vector>

int main() {
    using namespace mhf::host;

    // Defaults match the keys the game has always used.
    const KeyBindings defaults;
    MHF_CHECK(defaults.south == "KeyK");
    MHF_CHECK(defaults.east == "KeyL");
    MHF_CHECK(defaults.start == "Enter");
    MHF_CHECK(defaults.select == "Backspace");
    MHF_CHECK(defaults.stick_up == "KeyW");
    MHF_CHECK(kKeyActions.size() == 16);
    for (const std::string_view action : kKeyActions) {
        const std::string *value = key_binding(defaults, action);
        MHF_CHECK(value != nullptr && !value->empty());
    }
    MHF_CHECK(key_binding(defaults, "jump") == nullptr);

    // A keymap only changes what it names.
    const KeyBindings parsed = parse_keymap(" south = KeyZ , start=Space,stick_left=,jump=KeyQ ");
    MHF_CHECK(parsed.south == "KeyZ");
    MHF_CHECK(parsed.start == "Space");
    MHF_CHECK(parsed.stick_left.empty()); // an empty value is a button with no key
    MHF_CHECK(parsed.east == defaults.east);
    MHF_CHECK(parsed.stick_up == defaults.stick_up);
    MHF_CHECK(parse_keymap("") == defaults);
    MHF_CHECK(parse_keymap("nonsense") == defaults);

    // Formatting writes every action, so the game and the launcher always agree on all of them.
    const std::string text = format_keymap(parsed);
    MHF_CHECK(text.find("south=KeyZ") != std::string::npos);
    MHF_CHECK(text.find("stick_left=,") != std::string::npos);
    MHF_CHECK(format_keymap(parse_keymap(text)) == text);

    // UI Events codes to SDL scancodes (the launcher captures the former).
    MHF_CHECK(scancode_for_code("KeyK") == SDL_SCANCODE_K);
    MHF_CHECK(scancode_for_code("ArrowLeft") == SDL_SCANCODE_LEFT);
    MHF_CHECK(scancode_for_code("Digit4") == SDL_SCANCODE_4);
    MHF_CHECK(scancode_for_code("F5") == SDL_SCANCODE_F5);
    MHF_CHECK(scancode_for_code("Numpad7") == SDL_SCANCODE_KP_7);
    MHF_CHECK(scancode_for_code("Space") == SDL_SCANCODE_SPACE);
    MHF_CHECK(scancode_for_code("ShiftLeft") == SDL_SCANCODE_LSHIFT);
    MHF_CHECK(scancode_for_code("Keyk") == SDL_SCANCODE_UNKNOWN);
    MHF_CHECK(scancode_for_code("F13") == SDL_SCANCODE_UNKNOWN);
    MHF_CHECK(scancode_for_code("Nonsense") == SDL_SCANCODE_UNKNOWN);

    // Resolving: the defaults work, an unbound button stays unbound, a name we do not know falls back to its default.
    const KeyScancodes resolved = resolve_keymap(defaults);
    MHF_CHECK(resolved.south == SDL_SCANCODE_K);
    MHF_CHECK(resolved.start == SDL_SCANCODE_RETURN);
    MHF_CHECK(resolved.stick_right == SDL_SCANCODE_D);
    KeyBindings mixed = defaults;
    mixed.select.clear();
    mixed.north = "NotAKey";
    std::vector<std::string> unknown;
    const KeyScancodes partial = resolve_keymap(mixed, &unknown);
    MHF_CHECK(partial.select == SDL_SCANCODE_UNKNOWN);
    MHF_CHECK(partial.north == SDL_SCANCODE_I); // the default for Triangle
    MHF_CHECK(unknown.size() == 1 && unknown[0] == "NotAKey");

    // Every code the host knows maps back to the same scancode, and labels read like a keyboard.
    for (const char *code : {"KeyA", "KeyZ", "Digit0", "Digit1", "Digit9", "Numpad0", "Numpad7", "F1", "F12", "Enter", "ArrowLeft",
                             "ShiftRight", "Backquote", "NumpadEnter", "IntlBackslash"}) {
        const SDL_Scancode scancode = scancode_for_code(code);
        MHF_CHECK(scancode != SDL_SCANCODE_UNKNOWN);
        MHF_CHECK(code_for_scancode(scancode) == code);
    }
    MHF_CHECK(code_for_scancode(SDL_SCANCODE_UNKNOWN).empty());
    MHF_CHECK(key_label("KeyK") == "K");
    MHF_CHECK(key_label("Digit5") == "5");
    MHF_CHECK(key_label("ArrowUp") == "Up");
    MHF_CHECK(key_label("ShiftLeft") == "Left Shift");
    MHF_CHECK(key_label("NumpadEnter") == "Num Enter");
    MHF_CHECK(key_label("PageDown") == "Page Down");
    MHF_CHECK(key_label("").empty() == false && key_label("") == "Not set");

    return mhf::test::finish("host_keymap");
}
