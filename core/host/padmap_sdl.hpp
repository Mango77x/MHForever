#pragma once

// The SDL gamepad inputs behind a PadBindings: resolved once, when the bindings are set, and read on every poll
// (docs/07-launcher-ux.md section 5).

#include "host/padmap.hpp"

#include <SDL3/SDL_gamepad.h>

#include <array>
#include <string>
#include <vector>

namespace mhf::host {

// One physical input of a controller: a button, or a trigger counted as pressed past half its travel.
struct PadSource {
    bool trigger{};
    SDL_GamepadButton button{SDL_GAMEPAD_BUTTON_INVALID};
    SDL_GamepadAxis axis{SDL_GAMEPAD_AXIS_INVALID};
};

struct PadSources {
    std::array<PadSource, 4> list{};
    int count{};
};

struct PadMapping {
    // In the order of kPadActions, without the stick.
    std::array<PadSources, 12> buttons{};
    SDL_GamepadAxis stick_x{SDL_GAMEPAD_AXIS_INVALID}; // INVALID: no stick drives the PSP stick
    SDL_GamepadAxis stick_y{SDL_GAMEPAD_AXIS_INVALID};
};

// A name this host does not know falls back to that action's default and is added to `unknown`.
[[nodiscard]] PadMapping resolve_padmap(const PadBindings &pad, std::vector<std::string> *unknown = nullptr);

// Whether any source of `sources` is held on `gamepad`.
[[nodiscard]] bool pad_pressed(SDL_Gamepad *gamepad, const PadSources &sources);

} // namespace mhf::host
