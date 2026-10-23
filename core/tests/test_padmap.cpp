// Controller bindings: the names the launcher stores, and the SDL inputs the desktop host reads (no game data needed).

#include "host/padmap.hpp"
#include "host/padmap_sdl.hpp"

#include "check.hpp"

#include <string>
#include <vector>

int main() {
    using namespace mhf::host;

    // Defaults are what the game has always read from a controller.
    const PadBindings defaults;
    MHF_CHECK(defaults.south == "south");
    MHF_CHECK(defaults.select == "back");
    MHF_CHECK(defaults.left_shoulder == "left_shoulder+left_trigger");
    MHF_CHECK(defaults.stick == "left");
    MHF_CHECK(kPadActions.size() == 13);
    for (const std::string_view action : kPadActions) {
        const std::string *value = pad_binding(defaults, action);
        MHF_CHECK(value != nullptr && !value->empty());
    }
    MHF_CHECK(pad_binding(defaults, "jump") == nullptr);

    // A padmap only changes what it names; an empty value unbinds.
    const PadBindings parsed = parse_padmap(" south = east , start=guide,stick=,north=,jump=south ");
    MHF_CHECK(parsed.south == "east");
    MHF_CHECK(parsed.start == "guide");
    MHF_CHECK(parsed.stick.empty());
    MHF_CHECK(parsed.north.empty());
    MHF_CHECK(parsed.east == defaults.east);

    // Formatting then parsing gives the same bindings back, in the order of kPadActions.
    MHF_CHECK(parse_padmap(format_padmap(parsed)) == parsed);
    MHF_CHECK(format_padmap(defaults).rfind("south=south,east=east", 0) == 0);
    MHF_CHECK(parse_padmap(format_padmap(defaults)) == defaults);
    MHF_CHECK(parse_padmap("") == defaults);

    // Resolving: names become SDL inputs, an unbound button stays empty, an unknown name falls back to the default.
    PadBindings custom;
    custom.south = "dpad_up+right_trigger";
    custom.east = "";
    custom.west = "nonsense";
    custom.stick = "right";
    std::vector<std::string> unknown;
    const PadMapping mapping = resolve_padmap(custom, &unknown);
    MHF_CHECK(mapping.buttons[0].count == 2);
    MHF_CHECK(mapping.buttons[0].list[0].button == SDL_GAMEPAD_BUTTON_DPAD_UP);
    MHF_CHECK(mapping.buttons[0].list[1].trigger && mapping.buttons[0].list[1].axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    MHF_CHECK(mapping.buttons[1].count == 0);
    MHF_CHECK(mapping.buttons[2].count == 1 && mapping.buttons[2].list[0].button == SDL_GAMEPAD_BUTTON_WEST);
    MHF_CHECK(unknown.size() == 1 && unknown[0] == "nonsense");
    MHF_CHECK(mapping.stick_x == SDL_GAMEPAD_AXIS_RIGHTX && mapping.stick_y == SDL_GAMEPAD_AXIS_RIGHTY);

    const PadMapping standard = resolve_padmap(defaults);
    MHF_CHECK(standard.buttons[4].count == 2); // L: shoulder button or trigger
    MHF_CHECK(standard.stick_x == SDL_GAMEPAD_AXIS_LEFTX);
    PadBindings no_stick;
    no_stick.stick.clear();
    MHF_CHECK(resolve_padmap(no_stick).stick_x == SDL_GAMEPAD_AXIS_INVALID);

    return mhf::test::finish("host_padmap");
}
