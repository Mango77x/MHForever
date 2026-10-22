#include "overlay/menu.hpp"

#include "check.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace mhf::overlay;
using Key = Menu::Key;
using Kind = Menu::Event::Kind;

enum Id { kResume = 1, kMusic, kSmooth, kRate, kHidden, kQuit };

MenuView sample() {
    MenuView v;
    v.title = "Paused";
    MenuItem heading;
    heading.kind = MenuItem::Kind::Heading;
    heading.label = "Sound";
    MenuItem resume;
    resume.id = kResume;
    resume.label = "Resume";
    MenuItem music;
    music.kind = MenuItem::Kind::Slider;
    music.id = kMusic;
    music.label = "Music";
    music.value = 50;
    MenuItem smooth;
    smooth.kind = MenuItem::Kind::Toggle;
    smooth.id = kSmooth;
    smooth.label = "Smooth";
    MenuItem rate;
    rate.kind = MenuItem::Kind::Choice;
    rate.id = kRate;
    rate.label = "Frame rate";
    rate.options = {"30", "60", "90"};
    MenuItem hidden;
    hidden.id = kHidden;
    hidden.label = "Not now";
    hidden.enabled = false;
    MenuItem quit;
    quit.id = kQuit;
    quit.label = "Quit";
    quit.danger = true;
    v.items = {heading, resume, music, smooth, rate, hidden, quit};
    return v;
}

int selected_id(const Menu &m) { return m.view().items[static_cast<std::size_t>(m.view().selected)].id; }

} // namespace

int main() {
    Menu menu;
    menu.set(sample());

    // The selection starts on the first row that can be selected (not the heading).
    MHF_CHECK(selected_id(menu) == kResume);

    // Down and Up skip what cannot be selected, and wrap around.
    menu.press(Key::Down);
    MHF_CHECK(selected_id(menu) == kMusic);
    menu.press(Key::Down);
    menu.press(Key::Down);
    MHF_CHECK(selected_id(menu) == kRate);
    menu.press(Key::Down); // the disabled row is skipped
    MHF_CHECK(selected_id(menu) == kQuit);
    menu.press(Key::Down); // wraps to the top, past the heading
    MHF_CHECK(selected_id(menu) == kResume);
    menu.press(Key::Up);
    MHF_CHECK(selected_id(menu) == kQuit);

    // A button is activated by Accept only.
    auto e = menu.press(Key::Accept);
    MHF_CHECK(e.kind == Kind::Activated && e.id == kQuit);
    MHF_CHECK(menu.press(Key::Left).kind == Kind::None);

    // A slider moves by its step and stops at its ends.
    menu.press(Key::Up); // Rate (the disabled row is skipped)
    menu.press(Key::Up); // Smooth
    menu.press(Key::Up); // Music
    MHF_CHECK(selected_id(menu) == kMusic);
    e = menu.press(Key::Right);
    MHF_CHECK(e.kind == Kind::Changed && e.id == kMusic && e.value == 55);
    for (int i = 0; i < 20; ++i) menu.press(Key::Right);
    MHF_CHECK(menu.view().items[2].value == 100);
    MHF_CHECK(menu.press(Key::Right).kind == Kind::None); // nothing changed, nothing reported
    for (int i = 0; i < 30; ++i) menu.press(Key::Left);
    MHF_CHECK(menu.view().items[2].value == 0);
    MHF_CHECK(menu.press(Key::Accept).kind == Kind::None); // Accept does nothing on a slider

    // A toggle flips; a choice moves and wraps with Accept but not with the arrows.
    menu.press(Key::Down);
    e = menu.press(Key::Accept);
    MHF_CHECK(e.kind == Kind::Changed && e.id == kSmooth && e.value == 1);
    e = menu.press(Key::Left);
    MHF_CHECK(e.kind == Kind::Changed && e.value == 0);
    menu.press(Key::Down);
    MHF_CHECK(menu.press(Key::Left).kind == Kind::None); // first option already
    MHF_CHECK(menu.press(Key::Right).value == 1);
    MHF_CHECK(menu.press(Key::Right).value == 2);
    MHF_CHECK(menu.press(Key::Right).kind == Kind::None);
    MHF_CHECK(menu.press(Key::Accept).value == 0); // wraps

    // Back is reported; the version changes whenever the picture would.
    MHF_CHECK(menu.press(Key::Back).kind == Kind::Back);
    const auto version = menu.view().version;
    menu.press(Key::Down);
    MHF_CHECK(menu.view().version != version);

    // Rebuilding the rows keeps the selection on the same row.
    MenuView again = sample();
    again.items[2].value = 80; // the owner changed something
    menu.set(again);
    MHF_CHECK(selected_id(menu) == kQuit);
    MenuView fewer = sample();
    fewer.items.erase(fewer.items.begin() + 6); // Quit gone: back to the first selectable row
    menu.set(fewer);
    MHF_CHECK(selected_id(menu) == kResume);

    // An empty menu does not crash.
    Menu empty;
    empty.set(MenuView{});
    MHF_CHECK(empty.press(Key::Down).kind == Kind::None);
    MHF_CHECK(empty.press(Key::Accept).kind == Kind::None);

    // The renderer fills a canvas of the asked size and draws something.
    MenuRenderer renderer(MHF_FONT_DIR);
    std::vector<std::uint32_t> pixels;
    MHF_CHECK(renderer.render(sample(), 2, pixels));
    MHF_CHECK(pixels.size() == static_cast<std::size_t>(MenuRenderer::width(2)) * static_cast<std::size_t>(MenuRenderer::height(2)));
    MHF_CHECK(std::any_of(pixels.begin(), pixels.end(), [](std::uint32_t p) { return (p >> 24u) != 0; }));
    // The panel is on the left: the right edge of the canvas stays only dimmed backdrop (alpha 0.55), no panel colours.
    MHF_CHECK((pixels[static_cast<std::size_t>(MenuRenderer::width(2)) * 100u + 900u] >> 24u) < 200u);

    // MHF_MENU_DUMP=<file>: also writes the picture (raw premultiplied RGBA, 1920x1088) to look at it.
    if (const char *dump = std::getenv("MHF_MENU_DUMP")) {
        std::vector<std::uint32_t> big;
        // The rows of the game's menu, to see how it looks.
        MenuView v;
        v.title = "Paused";
        v.subtitle = "Freedom Unite";
        v.hint = "Arrows move  |  Enter select  |  Esc back";
        const auto row = [](MenuItem::Kind kind, int id, const char *label, int value) {
            MenuItem item;
            item.kind = kind;
            item.id = id;
            item.label = label;
            item.value = value;
            return item;
        };
        v.items = {row(MenuItem::Kind::Button, 1, "Resume", 0),     row(MenuItem::Kind::Heading, 0, "SOUND", 0),
                   row(MenuItem::Kind::Slider, 2, "Music", 80),     row(MenuItem::Kind::Slider, 3, "Sound effects", 90),
                   row(MenuItem::Kind::Heading, 0, "VIDEO", 0),     row(MenuItem::Kind::Toggle, 4, "Fullscreen", 1),
                   row(MenuItem::Kind::Toggle, 5, "Smooth textures", 0), row(MenuItem::Kind::Info, 0, "4x, 60 fps (set in launcher)", 0),
                   row(MenuItem::Kind::Button, 6, "Quit the game", 0)};
        v.items.back().danger = true;
        menu.set(v);
        menu.press(Key::Down); // on the music slider
        v = menu.view();
        renderer.render(v, 4, big);
        if (std::FILE *f = std::fopen(dump, "wb")) {
            std::fwrite(big.data(), 4, big.size(), f);
            std::fclose(f);
        }
    }

    return mhf::test::finish("test_menu");
}
