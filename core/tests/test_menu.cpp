#include "overlay/menu.hpp"
#include "overlay/painter.hpp"

#include "check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

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
    v.title = "MHForever";
    v.subtitle = "Freedom Unite";
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

    // Starts on the first section, on its first row, as for the mouse (no focus ring).
    MHF_CHECK(menu.view().tab == 0 && selected_id(menu) == kResume && !menu.view().tabs_focused && menu.view().pointer);

    // Down and Up move through the rows and wrap; a button is activated by Accept; a key leaves the pointer mode.
    menu.press(Key::Down);
    MHF_CHECK(selected_id(menu) == kQuit && !menu.view().pointer);
    menu.press(Key::Down);
    MHF_CHECK(selected_id(menu) == kResume);
    menu.press(Key::Down);
    auto e = menu.press(Key::Accept);
    MHF_CHECK(e.kind == Kind::Activated && e.id == kQuit && e.action == -1);
    menu.set_pointer(true);
    MHF_CHECK(menu.view().pointer);

    // Left from a button goes to the sidebar; there Up/Down pick the section and Right goes back into the rows.
    MHF_CHECK(menu.press(Key::Left).kind == Kind::None && menu.view().tabs_focused);
    menu.press(Key::Down);
    MHF_CHECK(menu.view().tab == 1 && menu.view().tabs_focused);
    MHF_CHECK(menu.press(Key::Right).kind == Kind::None && !menu.view().tabs_focused);
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

    // A choice moves with the arrows and wraps with Accept; Left before its first option goes to the sidebar. A toggle
    // flips with Accept.
    MHF_CHECK(menu.press(Key::Right).kind == Kind::None); // 4x is the last option
    e = menu.press(Key::Left);
    MHF_CHECK(e.kind == Kind::Changed && e.id == kScale && e.value == 2);
    MHF_CHECK(menu.press(Key::Accept).value == 3);
    MHF_CHECK(menu.press(Key::Accept).value == 0); // wraps
    MHF_CHECK(menu.press(Key::Left).kind == Kind::None && menu.view().tabs_focused);
    menu.press(Key::Right);
    MHF_CHECK(!menu.view().tabs_focused && selected_id(menu) == kScale);
    menu.press(Key::Down);
    MHF_CHECK(selected_id(menu) == kFps);
    menu.press(Key::Down);
    e = menu.press(Key::Accept);
    MHF_CHECK(e.kind == Kind::Changed && e.id == kFullscreen && e.value == 0);
    menu.press(Key::Down); // the disabled row is skipped
    MHF_CHECK(selected_id(menu) == kSmooth);
    MHF_CHECK(menu.press(Key::Accept).value == 1);

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
        MHF_CHECK(lay.tabs[0].y + lay.tabs[0].h <= lay.tabs[1].y && lay.tabs[1].y + lay.tabs[1].h <= lay.tabs[2].y); // a column

        // Five sections, as in the game's menu when it is online: they stay above the sidebar's button, left of the
        // panel; the button and the panel's × are hit as such.
        {
            MenuView five = sample();
            for (const char *name : {"Controls", "Online"}) five.tabs.push_back(MenuTab{name, {}});
            const MenuLayout wide = layouter.layout(five);
            MHF_CHECK(wide.tabs.size() == 5 && wide.resume.w > 0 && wide.close.w > 0);
            MHF_CHECK(wide.tabs.back().y + wide.tabs.back().h < wide.resume.y);
            MHF_CHECK(wide.tabs.front().x + wide.tabs.front().w < wide.close.x);
            MHF_CHECK(wide.hit(wide.resume.x + 4.0f, wide.resume.y + 4.0f).kind == MenuLayout::Hit::Kind::Resume);
            MHF_CHECK(wide.hit(wide.close.x + 4.0f, wide.close.y + 4.0f).kind == MenuLayout::Hit::Kind::Close);
        }

        // A tab is picked by clicking it.
        auto h = lay.hit(lay.tabs[2].x + 5.0f, lay.tabs[2].y + 5.0f);
        MHF_CHECK(h.kind == MenuLayout::Hit::Kind::Tab && h.index == 2);
        m.click_tab(h.index);
        MHF_CHECK(m.view().tab == 2 && !m.view().tabs_focused);
        lay = layouter.layout(m.view());

        // Over a row it is selected; a disabled row cannot be; a toggle flips; a choice's options are hit one by one.
        h = lay.hit(lay.rows[2].x + 10.0f, lay.rows[2].y + 5.0f);
        MHF_CHECK(h.kind == MenuLayout::Hit::Kind::Row && h.index == 2 && !h.on_bar);
        m.hover_row(h.index);
        MHF_CHECK(selected_id(m) == kFullscreen);
        e = m.click_row(2, h.along, false);
        MHF_CHECK(e.kind == Kind::Changed && e.id == kFullscreen && e.value == 0);
        MHF_CHECK(lay.options[0].size() == 4);
        h = lay.hit(lay.options[0][1].x + 2.0f, lay.options[0][1].y + 2.0f);
        MHF_CHECK(h.kind == MenuLayout::Hit::Kind::Option && h.index == 0 && h.sub == 1);
        MHF_CHECK(m.click_option(0, 3).kind == Kind::None); // 4x is chosen already
        e = m.click_option(0, h.sub);
        MHF_CHECK(e.kind == Kind::Changed && e.id == kScale && e.value == 1);
        MHF_CHECK(m.click_row(0, 0.5f, false).kind == Kind::None); // beside the options nothing changes
        m.hover_row(3);
        MHF_CHECK(selected_id(m) != kNotNow);

        // A slider: its track is hit separately, a click or a drag sets the value from the position (to a step), the
        // wheel over it moves it, and the wheel elsewhere moves the selection.
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
        MHF_CHECK(m.scroll(1, 1, layouter.layout(m.view())).value == 5);
        MHF_CHECK(m.view().tab == 1 && selected_id(m) == kMusic);
        MHF_CHECK(m.scroll(-1, -1, layouter.layout(m.view())).kind == Kind::None && selected_id(m) == kMusic); // elsewhere: no selection change

        // Out of range is ignored.
        m.hover_row(99);
        MHF_CHECK(m.click_row(99, 0.5f, false).kind == Kind::None);
        MHF_CHECK(m.drag_slider(-1, 0.5f).kind == Kind::None);
    }

    // The project's icon is drawn at the top of the sidebar.
    {
        MenuRenderer icon_renderer(MHF_FONT_DIR);
        std::vector<std::uint32_t> big;
        MHF_CHECK(icon_renderer.render(sample(), 4, big, false));
        const int w = MenuRenderer::width(4);
        bool any = false;
        for (int y = 18 * 4; y < 33 * 4; ++y) {
            for (int x = 17 * 4; x < 32 * 4; ++x) any = any || (big[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] >> 24u) != 0;
        }
        MHF_CHECK(any);
    }

    // A banner's and a text field's actions: Left/Right move between them (a field first, then its actions), Accept
    // reports the one focused, a click reports the one under the pointer; Left past the first goes to the sidebar.
    {
        MenuView v;
        MenuItem code = item(MenuItem::Kind::Banner, 50, "Your room code");
        code.text = "K7QF-29XM";
        code.actions = {"Copy", "New code"};
        MenuItem note = item(MenuItem::Kind::Banner, 51, "Nothing to press"); // no actions: never selected
        MenuItem join = item(MenuItem::Kind::Text, 52, "Join a friend's room");
        join.actions = {"Paste", "Join"};
        v.tabs = {MenuTab{"Game", {}}, MenuTab{"Online", {code, note, join}}};
        Menu m;
        m.set(v);
        m.click_tab(1);
        MHF_CHECK(selected_id(m) == 50 && m.view().action == 0);
        auto a = m.press(Key::Accept);
        MHF_CHECK(a.kind == Kind::Activated && a.id == 50 && a.action == 0);
        m.press(Key::Right);
        MHF_CHECK(m.press(Key::Accept).action == 1);
        m.press(Key::Right); // the last one: stays
        MHF_CHECK(m.view().action == 1);
        m.press(Key::Down); // the banner without actions is skipped
        MHF_CHECK(selected_id(m) == 52 && m.view().action == -1);
        a = m.press(Key::Accept);
        MHF_CHECK(a.kind == Kind::Activated && a.id == 52 && a.action == -1); // the field
        m.press(Key::Right);
        m.press(Key::Right);
        MHF_CHECK(m.press(Key::Accept).action == 1);
        m.press(Key::Left);
        m.press(Key::Left);
        MHF_CHECK(m.view().action == -1 && !m.view().tabs_focused);
        m.press(Key::Left);
        MHF_CHECK(m.view().tabs_focused);
        // The mouse: the layout has the buttons, a click on one is that action.
        MenuRenderer layouter(MHF_FONT_DIR);
        const MenuLayout lay = layouter.layout(m.view());
        MHF_CHECK(lay.actions.size() == 3 && lay.actions[0].size() == 2 && lay.actions[1].empty() && lay.actions[2].size() == 2);
        MHF_CHECK(lay.rows[1].w == 0);
        const auto h = lay.hit(lay.actions[2][0].x + 2.0f, lay.actions[2][0].y + 2.0f);
        MHF_CHECK(h.kind == MenuLayout::Hit::Kind::Action && h.index == 2 && h.sub == 0);
        a = m.click_action(h.index, h.sub);
        MHF_CHECK(a.kind == Kind::Activated && a.id == 52 && a.action == 0 && !m.view().tabs_focused);
        a = m.click_row(2, 0.1f, false);
        MHF_CHECK(a.kind == Kind::Activated && a.id == 52 && a.action == -1); // a click in the field
        MHF_CHECK(m.click_action(2, 5).kind == Kind::None);
    }

    // A text field's on-screen keyboard (a controller): it holds the focus, the arrows move across its keys (a shorter
    // last row spreads its keys, Up/Down wrap), Accept and a click report the key, Back closes it. It is drawn under
    // the field, over the rows below.
    {
        MenuView v;
        MenuItem join = item(MenuItem::Kind::Text, 60, "Join a friend's room");
        join.actions = {"Paste", "Join"};
        for (const char c : std::string("0123456789ABCDEFGHJKMNPQRSTVWXYZ")) join.keys.emplace_back(1, c);
        join.keys.emplace_back("Delete");
        join.keys.emplace_back("Join");
        MenuItem after = item(MenuItem::Kind::Button, 61, "Below");
        v.tabs = {MenuTab{"Online", {item(MenuItem::Kind::Button, 59, "Above"), join, after}}};
        Menu m;
        m.set(v);
        MHF_CHECK(m.keypad_row() == 1 && selected_id(m) == 60 && m.view().key == 0);
        m.press(Key::Right);
        m.press(Key::Right);
        auto a = m.press(Key::Accept);
        MHF_CHECK(a.kind == Kind::Key && a.id == 60 && a.value == 2);
        m.press(Key::Left);
        m.press(Key::Left);
        m.press(Key::Left); // the row's first key: stays
        MHF_CHECK(m.view().key == 0);
        m.press(Key::Up); // wraps to the last row: Delete under the left half
        MHF_CHECK(m.view().key == 32);
        m.press(Key::Right);
        MHF_CHECK(m.view().key == 33);
        m.press(Key::Up); // from Join (the right half) to the row above's right half
        MHF_CHECK(m.view().key >= 28 && m.view().key <= 31);
        m.press(Key::NextTab);
        MHF_CHECK(m.view().tab == 0 && selected_id(m) == 60); // the sections stay while it is up
        m.press(Key::Accept);
        // A rebuild keeps the focused key.
        const int key = m.view().key;
        m.set(v);
        MHF_CHECK(m.view().key == key && selected_id(m) == 60);
        MHF_CHECK(m.press(Key::Back).kind == Kind::Back);
        // The mouse: the keys are laid out over the rows below the field; a click reports the key.
        MenuRenderer layouter(MHF_FONT_DIR);
        const MenuLayout lay = layouter.layout(m.view());
        MHF_CHECK(lay.keypad_row == 1 && lay.keys.size() == 34);
        MHF_CHECK(lay.keypad.y > lay.rows[1].y && lay.keypad.w < 200.0f);
        MHF_CHECK(lay.keys[33].w > lay.keys[0].w * 3.0f);
        const auto h = lay.hit(lay.keys[5].x + 2.0f, lay.keys[5].y + 2.0f);
        MHF_CHECK(h.kind == MenuLayout::Hit::Kind::Key && h.sub == 5);
        const auto between = lay.hit(lay.keypad.x + 1.0f, lay.keypad.y + 1.0f);
        MHF_CHECK(between.kind == MenuLayout::Hit::Kind::Keypad);
        a = m.click_key(h.sub);
        MHF_CHECK(a.kind == Kind::Key && a.value == 5 && m.view().key == 5);
        // Closed (no keys), the arrows move between the rows again.
        v.tabs[0].items[1].keys.clear();
        m.set(v);
        MHF_CHECK(m.keypad_row() < 0);
        m.press(Key::Down);
        MHF_CHECK(selected_id(m) == 61);
        MHF_CHECK(layouter.layout(m.view()).keys.empty());
    }

    // A friend's row: picked only when it has buttons, its buttons focused from the first one, a click reports the one.
    {
        MenuView v;
        MenuItem away = item(MenuItem::Kind::Person, 70, "Away");
        MenuItem here = item(MenuItem::Kind::Person, 71, "Rathian", 1);
        here.detail = "Playing, private room K7QF-29XM on Socom";
        here.actions = {"Join", "Invite"};
        v.tabs = {MenuTab{"Online", {away, here}}};
        Menu m;
        m.set(v);
        MHF_CHECK(selected_id(m) == 71 && m.view().action == 0);
        m.press(Key::Right);
        auto a = m.press(Key::Accept);
        MHF_CHECK(a.kind == Kind::Activated && a.id == 71 && a.action == 1);
        MenuRenderer layouter(MHF_FONT_DIR);
        const MenuLayout lay = layouter.layout(m.view());
        MHF_CHECK(lay.rows[0].w == 0 && lay.actions[1].size() == 2);
        const auto h = lay.hit(lay.actions[1][0].x + 2.0f, lay.actions[1][0].y + 2.0f);
        MHF_CHECK(h.kind == MenuLayout::Hit::Kind::Action && h.index == 1 && h.sub == 0);
    }

    // A long section scrolls: the wheel moves the rows by a step (never past either end) and leaves the selection, rows
    // partly out of view are drawn clipped and picked only where they show, and keys bring the selection back into view.
    {
        MenuView v;
        MenuTab longer{"Long", {}};
        for (int i = 0; i < 30; ++i) longer.items.push_back(item(MenuItem::Kind::Button, 100 + i, "Row"));
        v.tabs = {longer};
        Menu m;
        m.set(v);
        MenuRenderer layouter(MHF_FONT_DIR);
        MenuLayout lay = layouter.layout(m.view());
        MHF_CHECK(lay.scroll == 0.0f && lay.max_scroll > 100.0f);
        m.set_pointer(true);
        m.scroll(-1, -1, lay);
        lay = layouter.layout(m.view());
        const float one = lay.scroll;
        MHF_CHECK(one > 10.0f && one < 40.0f && selected_id(m) == 100);
        for (int i = 0; i < 100; ++i) {
            m.scroll(-1, -1, lay);
            lay = layouter.layout(m.view());
        }
        MHF_CHECK(lay.scroll == lay.max_scroll);
        bool partly = false; // a row cut by the top edge: picked only below it
        for (const auto &r : lay.rows) {
            if (r.w > 0 && r.y < lay.content.y) {
                partly = true;
                MHF_CHECK(lay.hit(r.x + 2.0f, r.y + 0.5f).kind == MenuLayout::Hit::Kind::None);
            }
        }
        (void)partly;
        m.press(Key::Down); // keys: the selection (the second row) comes into view again
        lay = layouter.layout(m.view());
        MHF_CHECK(selected_id(m) == 101 && lay.scroll < one + 1.0f);
        m.sync_scroll(lay.scroll);
        m.set_pointer(true); // the mouse takes over where the keys left the rows
        MHF_CHECK(layouter.layout(m.view()).scroll == lay.scroll);
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
    // A notice: a card at the top centre, the rest of the picture clear.
    {
        std::vector<std::uint32_t> notice;
        MHF_CHECK(renderer.render_notice("Rathian is in your room", "Private room AB12-CD34: you meet in the Gathering Hall.", 960, 544, notice));
        MHF_CHECK(notice.size() == 960u * 544u);
        MHF_CHECK((notice[static_cast<std::size_t>(40 * 960 + 480)] >> 24u) != 0);    // the card
        MHF_CHECK(notice[static_cast<std::size_t>(400 * 960 + 480)] == 0u && notice[0] == 0u); // the game shows through
    }
    // Without the backdrop (the display blurs the game behind it) the corners stay clear.
    MHF_CHECK(renderer.render(shown.view(), 2, pixels, false));
    MHF_CHECK(pixels[0] == 0u && pixels[pixels.size() - 1] == 0u);

    // MHF_MENU_DUMP=<file>: also writes pictures (raw premultiplied RGBA, 1920x1088) to look at: <file> the Online
    // section as the game shows it, <file>.video the Video section with the focus ring of the keys, <file>.keypad the
    // code field's on-screen keyboard.
    if (const char *dump = std::getenv("MHF_MENU_DUMP")) {
        std::vector<std::uint32_t> big;
        MenuView v = sample();
        v.tabs.push_back(MenuTab{"Controls", {item(MenuItem::Kind::Button, 90, "Restore keyboard defaults")}});
        MenuItem halls = item(MenuItem::Kind::Choice, 91, "");
        halls.options = {"Public halls", "Private room"};
        halls.option_details = {"Everyone on the server, PPSSPP and PSP players too.", "Only friends with your room code, on MHForever."};
        halls.value = 1;
        MenuItem code = item(MenuItem::Kind::Banner, 92, "Your room code");
        code.text = "K7QF-29XM";
        code.actions = {"Copy", "New code"};
        code.primary_action = 0;
        MenuItem join = item(MenuItem::Kind::Text, 93, "Join a friend's room");
        join.placeholder = "Type or paste a code, like K7QF-29XM";
        join.actions = {"Paste", "Join"};
        join.primary_action = 1;
        MenuItem server = item(MenuItem::Kind::Choice, 94, "Server");
        server.options = {"Socom", "MGN", "Games Nexus", "Psi-Hate", "Neko PSP"};
        server.detail = "France, PPSSPP's default and the busiest";
        MenuItem rathian = item(MenuItem::Kind::Person, 95, "Rathian", 1);
        rathian.detail = "Playing, private room AB12-CD34 on Socom";
        rathian.actions = {"Join", "Invite"};
        rathian.primary_action = 0;
        v.tabs.push_back(MenuTab{"Online", {halls, code, join, server,
                                            item(MenuItem::Kind::Info, 0, "Next time you enter the Gathering Hall you see the halls of your room."),
                                            item(MenuItem::Kind::Heading, 0, "Friends"), rathian},
                                 "Who you meet in the Gathering Hall.", "Private"});
        Menu look;
        look.set(v);
        look.click_tab(4); // the Online section, as the mouse leaves it
        renderer.render(look.view(), 4, big, false);
        if (std::FILE *f = std::fopen(dump, "wb")) {
            std::fwrite(big.data(), 4, big.size(), f);
            std::fclose(f);
        }
        look.click_tab(2);
        look.press(Key::Down); // keys: the focus ring on the frame rate
        renderer.render(look.view(), 4, big, false);
        if (std::FILE *f = std::fopen((std::string(dump) + ".video").c_str(), "wb")) {
            std::fwrite(big.data(), 4, big.size(), f);
            std::fclose(f);
        }
        MenuItem &field = v.tabs[4].items[2];
        field.text = "K7QF-2";
        field.value = 1;
        for (const char c : std::string("0123456789ABCDEFGHJKMNPQRSTVWXYZ")) field.keys.emplace_back(1, c);
        field.keys.emplace_back("Delete");
        field.keys.emplace_back("Join");
        look.click_tab(4);
        look.set(v);
        look.press(Key::Right); // keys: the focus ring on a key
        renderer.render(look.view(), 4, big, false);
        if (std::FILE *f = std::fopen((std::string(dump) + ".keypad").c_str(), "wb")) {
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

    // Japanese text (the Japanese release's save titles) comes from the second font, at the main font's em, and wraps
    // between characters since it has no spaces.
    {
        namespace fs = std::filesystem;
        const fs::path fonts = fs::temp_directory_path() / "mhf_test_menu_fonts";
        fs::create_directories(fonts);
        fs::copy_file(fs::path(MHF_FONT_DIR) / "LiberationSans-Regular.ttf", fonts / "LiberationSans-Regular.ttf",
                      fs::copy_options::overwrite_existing);
        fs::copy_file(MHF_JAPANESE_FONT, fonts / "NotoSansJP-Regular.otf", fs::copy_options::overwrite_existing);
        Painter latin_only(MHF_FONT_DIR), painter(fonts);
        const std::string kana = "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86"; // あいう
        MHF_CHECK(std::abs(latin_only.measure(kana, 20.0f) - latin_only.measure("???", 20.0f)) < 0.01f); // shown as '?'
        const float width = painter.measure(kana, 20.0f);
        // Full-width: about one em each; the size is the line height, so the em of Liberation Sans is ~17.4 px at 20.
        MHF_CHECK(width > 3 * 16.0f && width < 3 * 19.0f);
        std::vector<std::uint32_t> a, b;
        painter.begin(a, 120, 40, 1.0f);
        painter.text(4, 28, kana, 20.0f, kWhite);
        painter.end();
        painter.begin(b, 120, 40, 1.0f);
        painter.text(4, 28, "???", 20.0f, kWhite);
        painter.end();
        MHF_CHECK(a != b && std::any_of(a.begin(), a.end(), [](std::uint32_t p) { return p != 0; }));
        std::string line;
        for (int i = 0; i < 10; ++i) line += kana;
        const auto lines = painter.wrap(line, 20.0f, 200.0f);
        MHF_CHECK(lines.size() >= 3);
        for (const auto &l : lines) MHF_CHECK(painter.measure(l, 20.0f) <= 200.0f);
        std::error_code ec;
        fs::remove_all(fonts, ec);
    }

    return mhf::test::finish("test_menu");
}
