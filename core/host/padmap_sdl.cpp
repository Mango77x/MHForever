#include "host/padmap_sdl.hpp"

#include <string_view>
#include <utility>

namespace mhf::host {
namespace {

constexpr std::pair<std::string_view, SDL_GamepadButton> kButtons[] = {
    {"south", SDL_GAMEPAD_BUTTON_SOUTH},
    {"east", SDL_GAMEPAD_BUTTON_EAST},
    {"west", SDL_GAMEPAD_BUTTON_WEST},
    {"north", SDL_GAMEPAD_BUTTON_NORTH},
    {"left_shoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"right_shoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"back", SDL_GAMEPAD_BUTTON_BACK},
    {"start", SDL_GAMEPAD_BUTTON_START},
    {"guide", SDL_GAMEPAD_BUTTON_GUIDE},
    {"left_stick", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"right_stick", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"dpad_up", SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"dpad_down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"dpad_left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"dpad_right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};

constexpr Sint16 kTriggerThreshold = 16000;

bool source_for(std::string_view name, PadSource &out) {
    out = {};
    if (name == "left_trigger" || name == "right_trigger") {
        out.trigger = true;
        out.axis = name == "left_trigger" ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        return true;
    }
    for (const auto &[button_name, button] : kButtons) {
        if (name == button_name) {
            out.button = button;
            return true;
        }
    }
    return false;
}

// "left_shoulder+left_trigger" -> two sources; an empty text is a binding with no source. False when a part is unknown.
bool parse_sources(std::string_view text, PadSources &out) {
    out = {};
    std::size_t start = 0;
    while (!text.empty() && start <= text.size()) {
        const std::size_t plus = text.find('+', start);
        const std::string_view part = text.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start);
        PadSource source;
        if (!source_for(part, source)) return false;
        if (out.count < static_cast<int>(out.list.size())) out.list[static_cast<std::size_t>(out.count++)] = source;
        if (plus == std::string_view::npos) break;
        start = plus + 1;
    }
    return true;
}

} // namespace

PadMapping resolve_padmap(const PadBindings &pad, std::vector<std::string> *unknown) {
    const PadBindings defaults;
    PadMapping out;
    for (std::size_t i = 0; i < out.buttons.size(); ++i) {
        const std::string_view action = kPadActions[i];
        const std::string *value = pad_binding(pad, action);
        if (value != nullptr && parse_sources(*value, out.buttons[i])) continue;
        if (value != nullptr && unknown != nullptr) unknown->push_back(*value);
        out.buttons[i] = {};
        if (const std::string *fallback = pad_binding(defaults, action)) parse_sources(*fallback, out.buttons[i]);
    }
    if (pad.stick == "right") {
        out.stick_x = SDL_GAMEPAD_AXIS_RIGHTX;
        out.stick_y = SDL_GAMEPAD_AXIS_RIGHTY;
    } else if (!pad.stick.empty()) { // "left", or a name this host does not know
        if (pad.stick != "left" && unknown != nullptr) unknown->push_back(pad.stick);
        out.stick_x = SDL_GAMEPAD_AXIS_LEFTX;
        out.stick_y = SDL_GAMEPAD_AXIS_LEFTY;
    }
    return out;
}

bool pad_pressed(SDL_Gamepad *gamepad, const PadSources &sources) {
    for (int i = 0; i < sources.count; ++i) {
        const PadSource &s = sources.list[static_cast<std::size_t>(i)];
        if (s.trigger ? SDL_GetGamepadAxis(gamepad, s.axis) > kTriggerThreshold : SDL_GetGamepadButton(gamepad, s.button)) return true;
    }
    return false;
}

} // namespace mhf::host
