#include "overlay/menu.hpp"

#include "check.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace mhf::overlay;
using Key = Menu::Key;
using Kind = Menu::Event::Kind;

enum Id { kResume = 1, kQuit, kMusic, kSfx, kScale, kFps, kFullscreen, kSmooth, kNotNow };

MenuItem item(MenuItem::Kind kind, int id, const char *label, int value = 0) {
    MenuItem i;
    i.kind = kind;
    i.id = id;
    i.label = label;
    i.value = value;
    return i;
}

MenuView sample() {
    MenuView v;
    v.title = "Freedom Unite";
    v.corner = "Esc  Resume";
    v.hint = "Arrows move  |  Enter select  |  Q E tabs  |  Esc resume";

    MenuTab game{"Game", {item(MenuItem::Kind::Button, kResume, "Resume"), item(MenuItem::Kind::Button, kQuit, "Quit the game")}};
    game.items[1].danger = true;

    MenuItem music = item(MenuItem::Kind::Slider, kMusic, "Music", 50);
    MenuTab audio{"Audio", {item(MenuItem::Kind::Heading, 0, "Volume"), music, item(MenuItem::Kind::Slider, kSfx, "Sound effects", 90)}};

    MenuItem scale = item(MenuItem::Kind::Choice, kScale, "Resolution");
    scale.options = {"1x", "2x", "3x", "4x"};
    scale.value = 3;
    MenuItem fps = item(MenuItem::Kind::Choice, kFps, "Frame rate");
    fps.options = {"30", "60", "90"};
    MenuItem hidden = item(MenuItem::Kind::Button, kNotNow, "Not now");
    hidden.enabled = false;
    MenuTab video{"Video", {scale, fps, item(MenuItem::Kind::Toggle, kFullscreen, "Fullscreen", 1), hidden,
                            item(MenuItem::Kind::Toggle, kSmooth, "Smooth textures", 0)}};
    v.tabs = {game, audio, video};
    return v;
}

int selected_id(const Menu &m) { return m.view().items()[static_cast<std::size_t>(m.view().selected)].id; }

} // namespace

int main() {
    Menu menu;
    menu.set(sample());

    // Starts on the first tab, on its first row.
    MHF_CHECK(menu.view().tab == 0 && selected_id(menu) == kResume && !menu.view().tabs_focused);

    // Down and Up move through the rows and wrap; a button is activated by Accept only.
    menu.press(Key::Down);
    MHF_CHECK(selected_id(menu) == kQuit);
    menu.press(Key::Down);
    MHF_CHECK(selected_id(menu) == kResume);
    menu.press(Key::Down);
    auto e = menu.press(Key::Accept);
    MHF_CHECK(e.kind == Kind::Activated && e.id == kQuit);
    MHF_CHECK(menu.press(Key::Left).kind == Kind::None);

    // Up from the first row goes to the tab bar; there Left/Right pick the tab and Down goes back into the rows.
    menu.press(Key::Up);
    MHF_CHECK(selected_id(menu) == kResume);
    menu.press(Key::Up);
    MHF_CHECK(menu.view().tabs_focused);
    menu.press(Key::Right);
    MHF_CHECK(menu.view().tab == 1 && menu.view().tabs_focused);
    MHF_CHECK(menu.press(Key::Accept).kind == Kind::None && !menu.view().tabs_focused);
    MHF_CHECK(selected_id(menu) == kMusic); // the heading is skipped

    // A slider moves by its step and stops at its ends, reporting only what changed.
    e = menu.press(Key::Right);
    MHF_CHECK(e.kind == Kind::Changed && e.id == kMusic && e.value == 55);
    for (int i = 0; i < 20; ++i) menu.press(Key::Right);
    MHF_CHECK(menu.view().items()[1].value == 100);
    MHF_CHECK(menu.press(Key::Right).kind == Kind::None);
    for (int i = 0; i < 30; ++i) menu.press(Key::Left);
    MHF_CHECK(menu.view().items()[1].value == 0);
    MHF_CHECK(menu.press(Key::Accept).kind == Kind::None); // Accept does nothing on a slider

    // The tab keys work from anywhere and wrap.
    menu.press(Key::NextTab);
    MHF_CHECK(menu.view().tab == 2 && selected_id(menu) == kScale);
    menu.press(Key::NextTab);
    MHF_CHECK(menu.view().tab == 0);
    menu.press(Key::PrevTab);
    MHF_CHECK(menu.view().tab == 2);

    // A choice moves with the arrows (not past its ends) and wraps with Accept; a toggle flips.
    MHF_CHECK(menu.press(Key::Right).kind == Kind::None); // 4x is the last option
    e = menu.press(Key::Left);
    MHF_CHECK(e.kind == Kind::Changed && e.id == kScale && e.value == 2);
    MHF_CHECK(menu.press(Key::Accept).value == 3);
    MHF_CHECK(menu.press(Key::Accept).value == 0); // wraps
    menu.press(Key::Down);
    MHF_CHECK(selected_id(menu) == kFps);
    menu.press(Key::Down);
    e = menu.press(Key::Accept);
    MHF_CHECK(e.kind == Kind::Changed && e.id == kFullscreen && e.value == 0);
    menu.press(Key::Down); // the disabled row is skipped
    MHF_CHECK(selected_id(menu) == kSmooth);
    MHF_CHECK(menu.press(Key::Right).value == 1);

    // Back is reported; the version changes whenever the picture would.
    MHF_CHECK(menu.press(Key::Back).kind == Kind::Back);
    const auto version = menu.view().version;
    menu.press(Key::Up);
    MHF_CHECK(menu.view().version != version);

    // Rebuilding the view keeps the tab and the row; a row that is gone falls back to the first one.
    MenuView again = sample();
    menu.set(again);
    MHF_CHECK(menu.view().tab == 2 && selected_id(menu) == kFullscreen);
    MenuView fewer = sample();
    fewer.tabs[2].items.erase(fewer.tabs[2].items.begin() + 2);
    menu.set(fewer);
    MHF_CHECK(menu.view().tab == 2 && selected_id(menu) == kScale);

    // Nothing to navigate: no crash.
    Menu empty;
    empty.set(MenuView{});
    MHF_CHECK(empty.press(Key::Down).kind == Kind::None);
    MHF_CHECK(empty.press(Key::Accept).kind == Kind::None);
    MHF_CHECK(empty.press(Key::NextTab).kind == Kind::None);

    // The renderer fills a canvas of the asked size and draws something.
    MenuRenderer renderer(MHF_FONT_DIR);
    std::vector<std::uint32_t> pixels;
    Menu shown;
    shown.set(sample());
    MHF_CHECK(renderer.render(shown.view(), 2, pixels));
    MHF_CHECK(pixels.size() == static_cast<std::size_t>(MenuRenderer::width(2)) * static_cast<std::size_t>(MenuRenderer::height(2)));
    MHF_CHECK(std::any_of(pixels.begin(), pixels.end(), [](std::uint32_t p) { return (p >> 24u) != 0; }));
    // Without the backdrop (the display blurs the game behind it) the corners stay clear.
    MHF_CHECK(renderer.render(shown.view(), 2, pixels, false));
    MHF_CHECK(pixels[0] == 0u && pixels[pixels.size() - 1] == 0u);

    // MHF_MENU_DUMP=<file>: also writes the picture (raw premultiplied RGBA, 1920x1088) to look at it.
    if (const char *dump = std::getenv("MHF_MENU_DUMP")) {
        std::vector<std::uint32_t> big;
        Menu look;
        look.set(sample());
        look.press(Key::NextTab);
        look.press(Key::NextTab); // the Video tab
        look.press(Key::Down);
        renderer.render(look.view(), 4, big, false);
        if (std::FILE *f = std::fopen(dump, "wb")) {
            std::fwrite(big.data(), 4, big.size(), f);
            std::fclose(f);
        }
    }

    return mhf::test::finish("test_menu");
}
