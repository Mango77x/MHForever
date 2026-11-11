#pragma once

// The scancodes behind a KeyBindings: the desktop host reads them every poll, so the names are resolved once, when the
// bindings are set (docs/07-launcher-ux.md section 5).

#include "host/keymap.hpp"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <string>
#include <vector>

namespace mhf::host {

struct KeyScancodes {
    SDL_Scancode south{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode east{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode west{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode north{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode left_shoulder{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode right_shoulder{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode start{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode select{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode up{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode down{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode left{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode right{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode stick_up{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode stick_down{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode stick_left{SDL_SCANCODE_UNKNOWN};
    SDL_Scancode stick_right{SDL_SCANCODE_UNKNOWN};
};

// A binding with no key stays SDL_SCANCODE_UNKNOWN. A name this host does not know falls back to that action's
// default and is added to `unknown`.
[[nodiscard]] KeyScancodes resolve_keymap(const KeyBindings &keys, std::vector<std::string> *unknown = nullptr);

// The mouse's buttons bind like keys: "Mouse1" (left), "Mouse2" (right), "Mouse3" (middle), "Mouse4" and "Mouse5" (the
// side buttons) stand for kMouseScancode + the SDL button, past every real scancode (never an index into the keyboard).
inline constexpr int kMouseScancode = SDL_SCANCODE_COUNT;
// The SDL mouse button of such a code (SDL_BUTTON_LEFT...), or 0 for a keyboard key.
[[nodiscard]] inline int mouse_button_of(SDL_Scancode scancode) {
    const int button = static_cast<int>(scancode) - kMouseScancode;
    return button >= 1 && button <= 5 ? button : 0;
}

// The scancode of one UI Events code ("KeyK", "ArrowLeft"...) or mouse button ("Mouse1"), or SDL_SCANCODE_UNKNOWN.
[[nodiscard]] SDL_Scancode scancode_for_code(std::string_view code);

// The other way round: the UI Events code of a scancode, or "" when the host has no name for it.
// The code of an SDL mouse button ("Mouse1" for SDL_BUTTON_LEFT), or "" for a button without one.
[[nodiscard]] std::string code_for_mouse_button(int button);
[[nodiscard]] std::string code_for_scancode(SDL_Scancode scancode);

} // namespace mhf::host
