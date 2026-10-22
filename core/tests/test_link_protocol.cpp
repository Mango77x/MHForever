#include "link/protocol.hpp"

#include "check.hpp"

using namespace mhf::link::protocol;

int main() {
    // The menu request carries the game window, negative positions included (a monitor to the left).
    const WindowRect window{-1920, 40, 1280, 720, false};
    const auto back = parse_menu(format_menu(window));
    MHF_CHECK(back && back->x == -1920 && back->y == 40 && back->width == 1280 && back->height == 720 && !back->fullscreen);
    const auto full = parse_menu("menu 0 0 1920 1080 1");
    MHF_CHECK(full && full->fullscreen);
    MHF_CHECK(!parse_menu("menu 0 0 0 1080 1"));
    MHF_CHECK(!parse_menu("menu 0 0 1920 1080 2"));
    MHF_CHECK(!parse_menu("menu 0 0 1920 1080"));
    MHF_CHECK(!parse_menu("menu a 0 1920 1080 1"));
    MHF_CHECK(!parse_menu("resume"));

    // Every command survives a round trip.
    for (const Command c : {Command{Command::Kind::SetMusic, 35}, Command{Command::Kind::SetSfx, 100},
                            Command{Command::Kind::Resume, 0},
                            Command{Command::Kind::Quit, 0}}) {
        const auto parsed = parse_command(format_command(c));
        MHF_CHECK(parsed && parsed->kind == c.kind && parsed->value == c.value);
    }

    // Anything else is refused.
    MHF_CHECK(!parse_command(""));
    MHF_CHECK(!parse_command("set music 101"));
    MHF_CHECK(!parse_command("set music -1"));
    MHF_CHECK(!parse_command("set music"));
    MHF_CHECK(!parse_command("set music 5x"));
    MHF_CHECK(!parse_command("set smooth 1"));
    MHF_CHECK(!parse_command("set volume 10"));
    MHF_CHECK(!parse_command("resume now"));
    MHF_CHECK(!parse_command("format C:"));

    return mhf::test::finish("test_link_protocol");
}
