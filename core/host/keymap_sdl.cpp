#include "host/keymap_sdl.hpp"

#include <string_view>
#include <utility>

namespace mhf::host {
namespace {

constexpr std::pair<std::string_view, SDL_Scancode> kNamedKeys[] = {
    {"Enter", SDL_SCANCODE_RETURN},
    {"Escape", SDL_SCANCODE_ESCAPE},
    {"Backspace", SDL_SCANCODE_BACKSPACE},
    {"Tab", SDL_SCANCODE_TAB},
    {"Space", SDL_SCANCODE_SPACE},
    {"Minus", SDL_SCANCODE_MINUS},
    {"Equal", SDL_SCANCODE_EQUALS},
    {"BracketLeft", SDL_SCANCODE_LEFTBRACKET},
    {"BracketRight", SDL_SCANCODE_RIGHTBRACKET},
    {"Backslash", SDL_SCANCODE_BACKSLASH},
    {"Semicolon", SDL_SCANCODE_SEMICOLON},
    {"Quote", SDL_SCANCODE_APOSTROPHE},
    {"Backquote", SDL_SCANCODE_GRAVE},
    {"Comma", SDL_SCANCODE_COMMA},
    {"Period", SDL_SCANCODE_PERIOD},
    {"Slash", SDL_SCANCODE_SLASH},
    {"CapsLock", SDL_SCANCODE_CAPSLOCK},
    {"Insert", SDL_SCANCODE_INSERT},
    {"Delete", SDL_SCANCODE_DELETE},
    {"Home", SDL_SCANCODE_HOME},
    {"End", SDL_SCANCODE_END},
    {"PageUp", SDL_SCANCODE_PAGEUP},
    {"PageDown", SDL_SCANCODE_PAGEDOWN},
    {"PrintScreen", SDL_SCANCODE_PRINTSCREEN},
    {"ScrollLock", SDL_SCANCODE_SCROLLLOCK},
    {"Pause", SDL_SCANCODE_PAUSE},
    {"ContextMenu", SDL_SCANCODE_APPLICATION},
    {"NumLock", SDL_SCANCODE_NUMLOCKCLEAR},
    {"ArrowUp", SDL_SCANCODE_UP},
    {"ArrowDown", SDL_SCANCODE_DOWN},
    {"ArrowLeft", SDL_SCANCODE_LEFT},
    {"ArrowRight", SDL_SCANCODE_RIGHT},
    {"ShiftLeft", SDL_SCANCODE_LSHIFT},
    {"ShiftRight", SDL_SCANCODE_RSHIFT},
    {"ControlLeft", SDL_SCANCODE_LCTRL},
    {"ControlRight", SDL_SCANCODE_RCTRL},
    {"AltLeft", SDL_SCANCODE_LALT},
    {"AltRight", SDL_SCANCODE_RALT},
    {"MetaLeft", SDL_SCANCODE_LGUI},
    {"MetaRight", SDL_SCANCODE_RGUI},
    {"NumpadAdd", SDL_SCANCODE_KP_PLUS},
    {"NumpadSubtract", SDL_SCANCODE_KP_MINUS},
    {"NumpadMultiply", SDL_SCANCODE_KP_MULTIPLY},
    {"NumpadDivide", SDL_SCANCODE_KP_DIVIDE},
    {"NumpadDecimal", SDL_SCANCODE_KP_PERIOD},
    {"NumpadEnter", SDL_SCANCODE_KP_ENTER},
    {"NumpadEqual", SDL_SCANCODE_KP_EQUALS},
    {"IntlBackslash", SDL_SCANCODE_NONUSBACKSLASH},
    {"IntlRo", SDL_SCANCODE_INTERNATIONAL1},
    {"IntlYen", SDL_SCANCODE_INTERNATIONAL3},
};

bool letter(char c) { return c >= 'A' && c <= 'Z'; }

} // namespace

SDL_Scancode scancode_for_code(std::string_view code) {
    if (code.size() == 4 && code.compare(0, 3, "Key") == 0 && letter(code[3])) {
        return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (code[3] - 'A'));
    }
    // SDL numbers the digit keys 1..9 and then 0, not 0..9.
    if (code.size() == 6 && code.compare(0, 5, "Digit") == 0 && code[5] >= '0' && code[5] <= '9') {
        const int digit = code[5] - '0';
        return digit == 0 ? SDL_SCANCODE_0 : static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (digit - 1));
    }
    if (code.size() == 7 && code.compare(0, 6, "Numpad") == 0 && code[6] >= '0' && code[6] <= '9') {
        const int digit = code[6] - '0';
        return digit == 0 ? SDL_SCANCODE_KP_0 : static_cast<SDL_Scancode>(SDL_SCANCODE_KP_1 + (digit - 1));
    }
    if (code.size() >= 2 && code[0] == 'F') { // F1..F12
        int n = 0;
        for (std::size_t i = 1; i < code.size(); ++i) {
            if (code[i] < '0' || code[i] > '9') return SDL_SCANCODE_UNKNOWN;
            n = n * 10 + (code[i] - '0');
        }
        if (n >= 1 && n <= 12) return static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (n - 1));
    }
    for (const auto &[name, scancode] : kNamedKeys) {
        if (code == name) return scancode;
    }
    return SDL_SCANCODE_UNKNOWN;
}

std::string code_for_scancode(SDL_Scancode scancode) {
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
        return std::string("Key") + static_cast<char>('A' + (scancode - SDL_SCANCODE_A));
    }
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) return "Digit" + std::to_string(scancode - SDL_SCANCODE_1 + 1);
    if (scancode == SDL_SCANCODE_0) return "Digit0";
    if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_9) return "Numpad" + std::to_string(scancode - SDL_SCANCODE_KP_1 + 1);
    if (scancode == SDL_SCANCODE_KP_0) return "Numpad0";
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) return "F" + std::to_string(scancode - SDL_SCANCODE_F1 + 1);
    for (const auto &[name, known] : kNamedKeys) {
        if (known == scancode) return std::string(name);
    }
    return {};
}

KeyScancodes resolve_keymap(const KeyBindings &keys, std::vector<std::string> *unknown) {
    const KeyBindings defaults;
    KeyScancodes out;
    const auto resolve = [&](std::string_view action, SDL_Scancode &field) {
        const std::string *value = key_binding(keys, action);
        if (value == nullptr || value->empty()) { // no such action (never) or the player left it unbound
            field = SDL_SCANCODE_UNKNOWN;
            return;
        }
        SDL_Scancode scancode = scancode_for_code(*value);
        if (scancode == SDL_SCANCODE_UNKNOWN) {
            if (unknown != nullptr) unknown->push_back(*value);
            const std::string *fallback = key_binding(defaults, action);
            scancode = fallback != nullptr ? scancode_for_code(*fallback) : SDL_SCANCODE_UNKNOWN;
        }
        field = scancode;
    };
    resolve("south", out.south);
    resolve("east", out.east);
    resolve("west", out.west);
    resolve("north", out.north);
    resolve("left_shoulder", out.left_shoulder);
    resolve("right_shoulder", out.right_shoulder);
    resolve("start", out.start);
    resolve("select", out.select);
    resolve("up", out.up);
    resolve("down", out.down);
    resolve("left", out.left);
    resolve("right", out.right);
    resolve("stick_up", out.stick_up);
    resolve("stick_down", out.stick_down);
    resolve("stick_left", out.stick_left);
    resolve("stick_right", out.stick_right);
    return out;
}

} // namespace mhf::host
