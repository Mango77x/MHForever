#include "overlay/menu.hpp"

#include "check.hpp"

#include <algorithm>
#include <cmath>
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

    // The mouse: positions come from the renderer's layout, and every action matches what the keys do.
    {
        MenuRenderer layouter(MHF_FONT_DIR);
        Menu m;
        m.set(sample());
        MenuLayout lay = layouter.layout(m.view());
        MHF_CHECK(lay.tabs.size() == 3 && lay.rows.size() == 2);
        MHF_CHECK(lay.tabs[0].x + lay.tabs[0].w < lay.tabs[1].x && lay.tabs[1].x + lay.tabs[1].w < lay.tabs[2].x); // apart

        // A tab is picked by clicking it.
        auto h = lay.hit(lay.tabs[2].x + 5.0f, lay.tabs[2].y + 5.0f);
        MHF_CHECK(h.kind == MenuLayout::Hit::Kind::Tab && h.index == 2);
        m.click_tab(h.index);
        MHF_CHECK(m.view().tab == 2 && !m.view().tabs_focused);
        lay = layouter.layout(m.view());

        // Over a row it is selected; a disabled row cannot be; a toggle flips and a choice steps by the side clicked.
        h = lay.hit(lay.rows[2].x + 10.0f, lay.rows[2].y + 5.0f);
        MHF_CHECK(h.kind == MenuLayout::Hit::Kind::Row && h.index == 2 && !h.on_bar);
        m.hover_row(h.index);
        MHF_CHECK(selected_id(m) == kFullscreen);
        e = m.click_row(2, h.along, false);
        MHF_CHECK(e.kind == Kind::Changed && e.id == kFullscreen && e.value == 0);
        MHF_CHECK(m.click_row(0, 0.9f, false).kind == Kind::None); // 4x is the last option already
        MHF_CHECK(m.click_row(0, 0.1f, false).value == 2);
        m.hover_row(3);
        MHF_CHECK(selected_id(m) != kNotNow);

        // A slider: its track is hit separately, a click or a drag sets the value from the position (to a step), the
        // wheel over it moves it, and the wheel elsewhere changes tab.
        m.click_tab(1);
        lay = layouter.layout(m.view());
        MHF_CHECK(lay.rows[0].w == 0 && lay.rows[1].w > 0 && lay.bars[1].w > 0); // the heading cannot be picked
        h = lay.hit(lay.bars[1].x + lay.bars[1].w * 0.5f, lay.rows[1].y + 3.0f);
        MHF_CHECK(h.kind == MenuLayout::Hit::Kind::Row && h.index == 1 && h.on_bar && std::fabs(h.along - 0.5f) < 0.02f);
        MHF_CHECK(m.click_row(1, h.along, false).kind == Kind::None); // off the track a click does not set it
        e = m.click_row(1, 0.8f, true);
        MHF_CHECK(e.kind == Kind::Changed && e.id == kMusic && e.value == 80);
        MHF_CHECK(m.drag_slider(1, 0.123f).value == 10); // snapped to the step of 5
        MHF_CHECK(m.drag_slider(1, 1.5f).value == 100);  // and kept in range
        MHF_CHECK(m.drag_slider(1, -1.0f).value == 0);
        MHF_CHECK(m.scroll(1, 1).value == 5);
        MHF_CHECK(m.view().tab == 1);
        MHF_CHECK(m.scroll(1, -1).kind == Kind::None && m.view().tab == 0); // up: the previous tab
        m.scroll(-1, -1);
        MHF_CHECK(m.view().tab == 1);

        // Out of range is ignored.
        m.hover_row(99);
        MHF_CHECK(m.click_row(99, 0.5f, false).kind == Kind::None);
        MHF_CHECK(m.drag_slider(-1, 0.5f).kind == Kind::None);
    }

    // The project's icon is drawn at the top left, and the same place stays clear without it.
    {
        MenuRenderer icon_renderer(MHF_FONT_DIR);
        std::vector<std::uint32_t> big;
        MHF_CHECK(icon_renderer.render(sample(), 4, big, false));
        const int w = MenuRenderer::width(4);
        bool any = false;
        for (int y = 15 * 4; y < 31 * 4; ++y) {
            for (int x = 26 * 4; x < 42 * 4; ++x) any = any || (big[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] >> 24u) != 0;
        }
        MHF_CHECK(any);
    }

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

    // A binding row is activated like a button, shows what it is set to, and the menu keeps the row selected when it is
    // rebuilt with the new assignment.
    {
        MenuView v;
        MenuItem bind = item(MenuItem::Kind::Binding, 40, "Cross");
        bind.text = "K";
        v.tabs = {MenuTab{"Controls", {item(MenuItem::Kind::Heading, 0, "Keyboard"), bind, item(MenuItem::Kind::Binding, 41, "Circle")}}};
        Menu menu;
        menu.set(v);
        MHF_CHECK(selected_id(menu) == 40); // the heading is skipped
        auto activated = menu.press(Key::Accept);
        MHF_CHECK(activated.kind == Kind::Activated && activated.id == 40);
        MHF_CHECK(menu.press(Key::Right).kind == Kind::None);
        menu.press(Key::Down);
        MHF_CHECK(selected_id(menu) == 41);
        v.tabs[0].items[1].text = "Space";
        v.tabs[0].items[2].value = 1;
        menu.set(v);
        MHF_CHECK(selected_id(menu) == 41 && menu.view().items()[1].text == "Space" && menu.view().items()[2].value == 1);
        const auto clicked = menu.click_row(1, 0.5f, false);
        MHF_CHECK(clicked.kind == Kind::Activated && clicked.id == 40);
    }

    return mhf::test::finish("test_menu");
}
