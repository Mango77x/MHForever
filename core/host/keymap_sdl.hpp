#pragma once

// The scancodes behind a KeyBindings: the desktop host reads them every poll, so the names are resolved once, when the
// bindings are set (docs/07-launcher-ux.md section 5).

#include "host/keymap.hpp"

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

// The scancode of one UI Events code ("KeyK", "ArrowLeft"...), or SDL_SCANCODE_UNKNOWN.
[[nodiscard]] SDL_Scancode scancode_for_code(std::string_view code);

// The other way round: the UI Events code of a scancode, or "" when the host has no name for it.
[[nodiscard]] std::string code_for_scancode(SDL_Scancode scancode);

} // namespace mhf::host
