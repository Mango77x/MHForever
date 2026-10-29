#include "overlay/menu.hpp"

#include "overlay/icon.hpp"
#include "overlay/painter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace mhf::overlay {

// ---------------------------------------------------------------- navigation

bool Menu::selectable(const MenuItem &item) {
    if (!item.enabled || item.kind == MenuItem::Kind::Info || item.kind == MenuItem::Kind::Heading) return false;
    return item.kind != MenuItem::Kind::Banner || !item.actions.empty();
}

int Menu::first_action(const MenuItem &item) { return item.kind == MenuItem::Kind::Banner && !item.actions.empty() ? 0 : -1; }

std::vector<MenuItem> &Menu::items() { return view_.tabs[static_cast<std::size_t>(view_.tab)].items; }

int Menu::first_selectable() const {
    const auto &rows = view_.items();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (selectable(rows[i])) return static_cast<int>(i);
    }
    return 0;
}

void Menu::set(MenuView view) {
    std::string keep_tab;
    int keep_id = -1, keep_action = -1;
    bool had_row = false;
    if (view_.tab >= 0 && view_.tab < static_cast<int>(view_.tabs.size())) {
        keep_tab = view_.tabs[static_cast<std::size_t>(view_.tab)].name;
        const auto &rows = view_.items();
        if (view_.selected >= 0 && view_.selected < static_cast<int>(rows.size())) {
            keep_id = rows[static_cast<std::size_t>(view_.selected)].id;
            keep_action = view_.action;
            had_row = true;
        }
    }
    const bool focus = view_.tabs_focused, pointer = view_.pointer;
    const int key = keypad_row() >= 0 ? view_.key : 0;
    view_ = std::move(view);
    view_.version = ++version_;
    view_.pointer = pointer;
    view_.key = key;
    view_.action = -1;
    if (view_.tabs.empty()) {
        view_.tab = 0;
        view_.selected = 0;
        view_.tabs_focused = false;
        return;
    }
    int tab = 0;
    for (std::size_t i = 0; i < view_.tabs.size(); ++i) {
        if (!keep_tab.empty() && view_.tabs[i].name == keep_tab) tab = static_cast<int>(i);
    }
    view_.tab = tab;
    view_.tabs_focused = focus && view_.tabs.size() > 1;
    int found = -1;
    const auto &rows = view_.items();
    for (std::size_t i = 0; i < rows.size() && found < 0; ++i) {
        if (had_row && rows[i].id == keep_id && selectable(rows[i])) found = static_cast<int>(i);
    }
    view_.selected = found >= 0 ? found : first_selectable();
    if (const int k = keypad_row(); k >= 0) {
        view_.selected = k; // the keyboard holds the focus
        view_.key = std::clamp(view_.key, 0, static_cast<int>(rows[static_cast<std::size_t>(k)].keys.size()) - 1);
    }
    if (!rows.empty()) {
        const MenuItem &row = rows[static_cast<std::size_t>(view_.selected)];
        view_.action = found >= 0 && keep_action < static_cast<int>(row.actions.size()) && (keep_action >= 0 || first_action(row) < 0)
                           ? keep_action
                           : first_action(row);
    }
}

void Menu::set_pointer(bool on) {
    if (view_.pointer == on) return;
    view_.pointer = on;
    view_.version = ++version_;
}

void Menu::move(int direction) {
    const auto &rows = view_.items();
    const int count = static_cast<int>(rows.size());
    for (int step = 1; step <= count; ++step) {
        const int i = ((view_.selected + direction * step) % count + count) % count;
        if (selectable(rows[static_cast<std::size_t>(i)])) {
            view_.selected = i;
            view_.action = first_action(rows[static_cast<std::size_t>(i)]);
            view_.version = ++version_;
            return;
        }
    }
}

void Menu::change_tab(int direction) {
    const int count = static_cast<int>(view_.tabs.size());
    if (count < 2) return;
    view_.tab = ((view_.tab + direction) % count + count) % count;
    view_.selected = first_selectable();
    const auto &rows = view_.items();
    view_.action = rows.empty() ? -1 : first_action(rows[static_cast<std::size_t>(view_.selected)]);
    view_.version = ++version_;
}

void Menu::focus_tabs() {
    if (view_.tabs.size() < 2 && view_.tabs_focused) return;
    view_.tabs_focused = true;
    view_.version = ++version_;
}

int Menu::keypad_row() const {
    const auto &rows = view_.items();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind == MenuItem::Kind::Text && !rows[i].keys.empty()) return static_cast<int>(i);
    }
    return -1;
}

namespace {

// Where key `k` is on an on-screen keyboard of `count` keys, `columns` a row: its row, and its centre across 0..1.
void key_place(int k, int count, int columns, int &row, float &centre) {
    row = k / columns;
    const int first = row * columns, in_row = std::min(columns, count - first);
    centre = (static_cast<float>(k - first) + 0.5f) / static_cast<float>(in_row);
}

} // namespace

void Menu::hover_key(int key) {
    const int k = keypad_row();
    if (k < 0 || key < 0 || key >= static_cast<int>(view_.items()[static_cast<std::size_t>(k)].keys.size()) || key == view_.key) return;
    view_.key = key;
    view_.version = ++version_;
}

Menu::Event Menu::click_key(int key) {
    const int k = keypad_row();
    if (k < 0 || key < 0 || key >= static_cast<int>(view_.items()[static_cast<std::size_t>(k)].keys.size())) return {};
    hover_key(key);
    return {Event::Kind::Key, view_.items()[static_cast<std::size_t>(k)].id, key};
}

Menu::Event Menu::press(Key key) {
    set_pointer(false);
    if (key == Key::Back) return {Event::Kind::Back, 0, 0};
    if (view_.tabs.empty()) return {};
    if (const int k = keypad_row(); k >= 0) { // the on-screen keyboard: the arrows move across its keys
        const MenuItem &item = view_.items()[static_cast<std::size_t>(k)];
        const int count = static_cast<int>(item.keys.size()), columns = std::max(item.key_columns, 1);
        const int last_row = (count - 1) / columns;
        int row = 0, at = view_.key;
        float centre = 0.0f;
        key_place(view_.key, count, columns, row, centre);
        switch (key) {
        case Key::Accept: return {Event::Kind::Key, item.id, view_.key};
        case Key::Left: at = std::max(view_.key - 1, row * columns); break;
        case Key::Right: at = std::min(view_.key + 1, std::min(count, (row + 1) * columns) - 1); break;
        case Key::Up:
        case Key::Down: {
            const int to = key == Key::Up ? (row == 0 ? last_row : row - 1) : (row == last_row ? 0 : row + 1);
            const int in_row = std::min(columns, count - to * columns);
            at = to * columns + std::clamp(static_cast<int>(centre * static_cast<float>(in_row)), 0, in_row - 1);
            break;
        }
        default: break;
        }
        if (at != view_.key) {
            view_.key = at;
            view_.version = ++version_;
        }
        return {};
    }
    if (key == Key::PrevTab || key == Key::NextTab) {
        change_tab(key == Key::PrevTab ? -1 : 1);
        return {};
    }
    if (view_.tabs_focused) { // the sidebar: Up/Down pick the section, Right (or Accept) goes into it
        if (key == Key::Up || key == Key::Down) change_tab(key == Key::Up ? -1 : 1);
        if (key == Key::Right || key == Key::Accept) {
            view_.tabs_focused = false;
            view_.version = ++version_;
        }
        return {};
    }
    auto &rows = items();
    if (rows.empty()) {
        if (key == Key::Left) focus_tabs();
        return {};
    }
    if (key == Key::Up || key == Key::Down) {
        move(key == Key::Up ? -1 : 1);
        return {};
    }
    if (view_.selected < 0 || view_.selected >= static_cast<int>(rows.size())) return {};
    MenuItem &item = rows[static_cast<std::size_t>(view_.selected)];
    if (!selectable(item)) return {};
    const auto changed = [&](int value) { return set_value(item, value); };
    const auto activated = [&](int action) { return Event{Event::Kind::Activated, item.id, 0, action}; };
    const int actions = static_cast<int>(item.actions.size());
    switch (item.kind) {
    case MenuItem::Kind::Button:
    case MenuItem::Kind::Binding:
        if (key == Key::Accept) return activated(-1);
        if (key == Key::Left) focus_tabs();
        return {};
    case MenuItem::Kind::Toggle:
        if (key == Key::Accept) return changed(item.value != 0 ? 0 : 1);
        if (key == Key::Left) focus_tabs();
        return {};
    case MenuItem::Kind::Slider:
        if (key == Key::Left || key == Key::Right) {
            return changed(std::clamp(item.value + (key == Key::Left ? -1 : 1) * std::max(item.step, 1), item.min, item.max));
        }
        return {};
    case MenuItem::Kind::Choice: {
        const int n = static_cast<int>(item.options.size());
        if (n == 0) return {};
        if (key == Key::Accept) return changed((item.value + 1) % n);
        if (key == Key::Right) return changed(std::min(item.value + 1, n - 1));
        if (key == Key::Left) {
            if (item.value <= 0) focus_tabs(); // past the first option: out to the sidebar
            else return changed(item.value - 1);
        }
        return {};
    }
    case MenuItem::Kind::Banner:
    case MenuItem::Kind::Text: {
        const int lowest = item.kind == MenuItem::Kind::Banner ? 0 : -1; // a field is focused before its actions
        if (key == Key::Accept) return activated(view_.action);
        if (key == Key::Left) {
            if (view_.action <= lowest) {
                focus_tabs();
            } else {
                --view_.action;
                view_.version = ++version_;
            }
        } else if (key == Key::Right && view_.action + 1 < actions) {
            ++view_.action;
            view_.version = ++version_;
        }
        return {};
    }
    default: return {};
    }
}

// ---------------------------------------------------------------- the mouse

MenuLayout::Hit MenuLayout::hit(float x, float y) const {
    Hit h;
    if (keypad.contains(x, y)) { // an open on-screen keyboard lies over the rows
        h.kind = Hit::Kind::Keypad;
        h.index = keypad_row;
        for (std::size_t k = 0; k < keys.size(); ++k) {
            if (keys[k].contains(x, y)) {
                h.kind = Hit::Kind::Key;
                h.sub = static_cast<int>(k);
            }
        }
        return h;
    }
    if (close.contains(x, y)) {
        h.kind = Hit::Kind::Close;
        return h;
    }
    if (resume.contains(x, y)) {
        h.kind = Hit::Kind::Resume;
        return h;
    }
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        if (tabs[i].contains(x, y)) {
            h.kind = Hit::Kind::Tab;
            h.index = static_cast<int>(i);
            return h;
        }
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].contains(x, y)) continue;
        h.index = static_cast<int>(i);
        if (i < actions.size()) {
            for (std::size_t a = 0; a < actions[i].size(); ++a) {
                if (actions[i][a].contains(x, y)) {
                    h.kind = Hit::Kind::Action;
                    h.sub = static_cast<int>(a);
                    return h;
                }
            }
        }
        if (i < options.size()) {
            for (std::size_t o = 0; o < options[i].size(); ++o) {
                if (options[i][o].contains(x, y)) {
                    h.kind = Hit::Kind::Option;
                    h.sub = static_cast<int>(o);
                    return h;
                }
            }
        }
        h.kind = Hit::Kind::Row;
        h.along = std::clamp((x - rows[i].x) / rows[i].w, 0.0f, 1.0f);
        if (i < bars.size() && bars[i].w > 0 && x >= bars[i].x - 4.0f && x < bars[i].x + bars[i].w + 4.0f) {
            h.on_bar = true;
            h.along = std::clamp((x - bars[i].x) / bars[i].w, 0.0f, 1.0f);
        }
        return h;
    }
    return h;
}

Menu::Event Menu::set_value(MenuItem &item, int value) {
    if (value == item.value) return {};
    item.value = value;
    view_.version = ++version_;
    return {Event::Kind::Changed, item.id, value};
}

void Menu::hover_row(int row, int action) {
    if (view_.tabs.empty() || row < 0 || row >= static_cast<int>(view_.items().size())) return;
    const MenuItem &item = view_.items()[static_cast<std::size_t>(row)];
    if (!selectable(item)) return;
    if (action < -1 || action >= static_cast<int>(item.actions.size())) action = -1;
    if (action < 0) action = first_action(item);
    if (view_.selected != row || view_.action != action || view_.tabs_focused) {
        view_.selected = row;
        view_.action = action;
        view_.tabs_focused = false;
        view_.version = ++version_;
    }
}

void Menu::click_tab(int tab) {
    if (tab < 0 || tab >= static_cast<int>(view_.tabs.size())) return;
    if (tab == view_.tab && !view_.tabs_focused) return;
    view_.tab = tab;
    view_.selected = first_selectable();
    const auto &rows = view_.items();
    view_.action = rows.empty() ? -1 : first_action(rows[static_cast<std::size_t>(view_.selected)]);
    view_.tabs_focused = false;
    view_.version = ++version_;
}

Menu::Event Menu::click_row(int row, float along, bool on_bar) {
    if (view_.tabs.empty() || row < 0 || row >= static_cast<int>(items().size())) return {};
    hover_row(row);
    MenuItem &item = items()[static_cast<std::size_t>(row)];
    if (!selectable(item)) return {};
    switch (item.kind) {
    case MenuItem::Kind::Button:
    case MenuItem::Kind::Binding: return {Event::Kind::Activated, item.id, 0, -1};
    case MenuItem::Kind::Text: return {Event::Kind::Activated, item.id, 0, -1}; // the field: start typing
    case MenuItem::Kind::Toggle: return set_value(item, item.value != 0 ? 0 : 1);
    case MenuItem::Kind::Slider:
        if (on_bar) return drag_slider(row, along);
        return {};
    default: return {};
    }
}

Menu::Event Menu::click_option(int row, int option) {
    if (view_.tabs.empty() || row < 0 || row >= static_cast<int>(items().size())) return {};
    hover_row(row);
    MenuItem &item = items()[static_cast<std::size_t>(row)];
    if (!selectable(item) || item.kind != MenuItem::Kind::Choice) return {};
    if (option < 0 || option >= static_cast<int>(item.options.size())) return {};
    return set_value(item, option);
}

Menu::Event Menu::click_action(int row, int action) {
    if (view_.tabs.empty() || row < 0 || row >= static_cast<int>(items().size())) return {};
    const MenuItem &item = items()[static_cast<std::size_t>(row)];
    if (!selectable(item) || action < 0 || action >= static_cast<int>(item.actions.size())) return {};
    hover_row(row, action);
    return {Event::Kind::Activated, item.id, 0, action};
}

Menu::Event Menu::drag_slider(int row, float along) {
    if (view_.tabs.empty() || row < 0 || row >= static_cast<int>(items().size())) return {};
    MenuItem &item = items()[static_cast<std::size_t>(row)];
    if (item.kind != MenuItem::Kind::Slider || !item.enabled) return {};
    const int step = std::max(item.step, 1);
    const float raw = static_cast<float>(item.min) + std::clamp(along, 0.0f, 1.0f) * static_cast<float>(item.max - item.min);
    const int snapped = item.min + static_cast<int>(std::lround((raw - static_cast<float>(item.min)) / static_cast<float>(step))) * step;
    return set_value(item, std::clamp(snapped, item.min, item.max));
}

Menu::Event Menu::scroll(int direction, int row) {
    if (view_.tabs.empty() || direction == 0) return {};
    if (row >= 0 && row < static_cast<int>(items().size())) {
        MenuItem &item = items()[static_cast<std::size_t>(row)];
        if (item.kind == MenuItem::Kind::Slider && item.enabled) {
            return set_value(item, std::clamp(item.value + direction * std::max(item.step, 1), item.min, item.max));
        }
    }
    if (!items().empty()) move(-direction); // the wheel up selects the row above, and the list follows
    return {};
}

// ---------------------------------------------------------------- drawing

namespace {

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

constexpr Color kPanel{0.094f, 0.067f, 0.106f};
constexpr Color kWarm{0.969f, 0.847f, 0.651f};   // text on an amber tint
constexpr Color kSoft{0.847f, 0.812f, 0.851f};   // labels that are not focused
constexpr Color kGreen{0.435f, 0.820f, 0.545f};
constexpr Color kBlue{0.561f, 0.722f, 0.910f};

} // namespace

struct MenuRenderer::Impl : Painter {
    explicit Impl(const std::filesystem::path &font_directory) : Painter(font_directory) {}

    // Logical units of the 480x272 canvas (the mockup at 1280 px wide, times 0.375).
    static constexpr float kW = 480.0f, kH = 272.0f;
    static constexpr float kPad = 10.0f, kGap = 7.0f, kRadius = 6.5f;
    static constexpr float kSideW = 88.0f;
    static constexpr float kNavTop = 33.0f, kNavH = 17.0f, kNavGap = 2.0f;
    static constexpr float kLabel = 5.8f, kDetail = 4.8f, kSmall = 4.4f;
    static constexpr float kRowGap = 3.5f, kCard = 4.5f;

    struct Panels {
        MenuLayout::Rect side, main, content;
    };

    static Panels panels(const MenuView &v) {
        Panels p;
        p.side = {kPad, kPad, kSideW, kH - 2.0f * kPad};
        p.main = {kPad + kSideW + kGap, kPad, kW - kPad * 2.0f - kSideW - kGap, kH - 2.0f * kPad};
        const float bottom = p.main.y + p.main.h - (!v.pointer && !v.hint.empty() ? 16.0f : 9.0f);
        p.content = {p.main.x + 14.0f, p.main.y + 37.0f, p.main.w - 28.0f, bottom - (p.main.y + 37.0f)};
        return p;
    }

    float row_height(const MenuItem &item, float w) const {
        const bool detail = !item.detail.empty();
        switch (item.kind) {
        case MenuItem::Kind::Heading: return 12.0f;
        case MenuItem::Kind::Info: return static_cast<float>(std::max<std::size_t>(1, wrap(item.label, 5.2f, w - 22.0f).size())) * 7.2f + 8.0f;
        case MenuItem::Kind::Choice:
            if (!item.option_details.empty()) return item.label.empty() ? 30.0f : 40.0f;
            return 31.0f;
        case MenuItem::Kind::Banner: return 30.0f;
        case MenuItem::Kind::Text: return 28.0f;
        default: return detail ? 26.0f : 20.0f;
        }
    }

    // Buttons laid right to left from `right`, at (y, h): the rects in the order given.
    std::vector<MenuLayout::Rect> buttons(const std::vector<std::string> &labels, float right, float y, float h) const {
        std::vector<MenuLayout::Rect> out(labels.size());
        float x = right;
        for (std::size_t i = labels.size(); i-- > 0;) {
            const float w = std::max(22.0f, measure(labels[i], 5.2f, true) + 13.0f);
            x -= w;
            out[i] = {x, y, w, h};
            x -= 3.5f;
        }
        return out;
    }

    struct Placed {
        MenuLayout layout;
        std::vector<float> row_y;   // every row's top, scrolled
        std::vector<bool> shown;
        Panels panels;
        float scroll = 0.0f, total = 0.0f;
    };

    Placed place(const MenuView &v) const {
        Placed p;
        p.panels = panels(v);
        const auto &side = p.panels.side;
        for (std::size_t i = 0; i < v.tabs.size(); ++i) {
            p.layout.tabs.push_back({side.x + 5.0f, side.y + kNavTop + static_cast<float>(i) * (kNavH + kNavGap), side.w - 10.0f, kNavH});
        }
        p.layout.resume = {side.x + 5.0f, side.y + side.h - 30.0f, side.w - 10.0f, 17.0f};
        p.layout.close = {p.panels.main.x + p.panels.main.w - 23.0f, p.panels.main.y + 6.0f, 17.0f, 17.0f};

        const auto &rows = v.items();
        const auto &c = p.panels.content;
        std::vector<float> offsets(rows.size() + 1, 0.0f);
        for (std::size_t i = 0; i < rows.size(); ++i) offsets[i + 1] = offsets[i] + row_height(rows[i], c.w) + kRowGap;
        p.total = offsets.back();
        if (v.selected >= 0 && v.selected < static_cast<int>(rows.size()) && p.total > c.h) {
            const auto s = static_cast<std::size_t>(v.selected);
            if (offsets[s + 1] - kRowGap > c.h) p.scroll = offsets[s + 1] - kRowGap - c.h;
            if (s > 0 && rows[s - 1].kind == MenuItem::Kind::Heading && offsets[s - 1] < p.scroll) p.scroll = offsets[s - 1];
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const MenuItem &item = rows[i];
            const float h = row_height(item, c.w);
            const float y = c.y + offsets[i] - p.scroll;
            const bool visible = y >= c.y - 0.5f && y + h <= c.y + c.h + 0.5f;
            p.row_y.push_back(y);
            p.shown.push_back(visible);
            const bool pickable = visible && Menu::selectable(item);
            p.layout.rows.push_back(pickable ? MenuLayout::Rect{c.x, y, c.w, h} : MenuLayout::Rect{});
            MenuLayout::Rect bar;
            std::vector<MenuLayout::Rect> options, actions;
            if (pickable) {
                switch (item.kind) {
                case MenuItem::Kind::Slider: {
                    const float bx = c.x + c.w * 0.42f;
                    bar = {bx, y + h * 0.5f - 3.0f, c.x + c.w - 30.0f - bx, 6.0f};
                    break;
                }
                case MenuItem::Kind::Choice:
                    if (!item.option_details.empty()) {
                        const float top = item.label.empty() ? y : y + 11.0f;
                        const float n = static_cast<float>(item.options.size());
                        const float w = (c.w - (n - 1.0f) * 4.0f) / std::max(n, 1.0f);
                        for (std::size_t o = 0; o < item.options.size(); ++o) options.push_back({c.x + static_cast<float>(o) * (w + 4.0f), top, w, h - (top - y)});
                    } else {
                        float x = c.x + 8.0f;
                        for (const std::string &o : item.options) {
                            const float w = std::max(16.0f, measure(o, 5.2f, true) + 9.0f);
                            options.push_back({x, y + 14.0f, w, 13.0f});
                            x += w + 2.2f;
                        }
                    }
                    break;
                case MenuItem::Kind::Banner: actions = buttons(item.actions, c.x + c.w - 8.0f, y + 7.0f, 16.0f); break;
                case MenuItem::Kind::Text: actions = buttons(item.actions, c.x + c.w, y + 9.0f, 17.0f); break;
                default: break;
                }
            }
            p.layout.bars.push_back(bar);
            p.layout.options.push_back(std::move(options));
            p.layout.actions.push_back(std::move(actions));
            if (item.kind == MenuItem::Kind::Text && !item.keys.empty() && p.layout.keypad_row < 0) place_keypad(p, i, item);
        }
        return p;
    }

    // The on-screen keyboard: a small panel under the field (over it when there is no room below), its keys in rows.
    static constexpr float kKeyW = 14.0f, kKeyH = 13.0f, kKeyGap = 2.5f, kKeypadPad = 6.0f;

    void place_keypad(Placed &p, std::size_t i, const MenuItem &item) const {
        const int count = static_cast<int>(item.keys.size()), columns = std::max(item.key_columns, 1);
        const int rows = (count + columns - 1) / columns;
        const float inner_w = static_cast<float>(columns) * kKeyW + static_cast<float>(columns - 1) * kKeyGap;
        const float w = inner_w + 2.0f * kKeypadPad;
        const float h = static_cast<float>(rows) * kKeyH + static_cast<float>(rows - 1) * kKeyGap + 2.0f * kKeypadPad;
        const auto &main = p.panels.main;
        const float field_top = p.row_y[i] + 9.0f, field_bottom = field_top + 17.0f;
        float y = field_bottom + 3.0f;
        if (y + h > main.y + main.h - 4.0f) y = std::max(main.y + 4.0f, field_top - 3.0f - h);
        const float x = p.panels.content.x;
        p.layout.keypad = {x, y, w, h};
        p.layout.keypad_row = static_cast<int>(i);
        for (int k = 0; k < count; ++k) {
            const int row = k / columns, first = row * columns, in_row = std::min(columns, count - first);
            const float kw = (inner_w - static_cast<float>(in_row - 1) * kKeyGap) / static_cast<float>(in_row);
            p.layout.keys.push_back({x + kKeypadPad + static_cast<float>(k - first) * (kw + kKeyGap),
                                     y + kKeypadPad + static_cast<float>(row) * (kKeyH + kKeyGap), kw, kKeyH});
        }
    }

    void keypad(const Placed &p, const MenuItem &item, const MenuView &v) {
        const auto &pad = p.layout.keypad;
        rrect(pad.x - 0.5f, pad.y + 1.5f, pad.w + 1.0f, pad.h + 1.0f, 6.0f, kBlack, 0.35f); // a soft shadow
        rrect(pad.x, pad.y, pad.w, pad.h, 6.0f, kPanel, 0.98f);
        rrect(pad.x, pad.y, pad.w, pad.h, 6.0f, kAccent, 0.45f, 0.6f);
        for (std::size_t k = 0; k < p.layout.keys.size() && k < item.keys.size(); ++k) {
            const auto &r = p.layout.keys[k];
            const bool focused = static_cast<int>(k) == v.key;
            const bool last = item.keys.size() > 1 && k + 1 == item.keys.size(); // the last key confirms
            if (last) rrect(r.x, r.y, r.w, r.h, 3.5f, kAccent, focused ? 1.0f : 0.85f);
            else rrect(r.x, r.y, r.w, r.h, 3.5f, kWhite, focused ? 0.2f : 0.07f);
            centered(r, item.keys[k], 5.6f, last ? kInk : kText, 1.0f, true);
            if (focused && !v.pointer) ring(r, 3.5f);
        }
    }

    void ring(const MenuLayout::Rect &r, float radius) { rrect(r.x - 1.2f, r.y - 1.2f, r.w + 2.4f, r.h + 2.4f, radius + 1.0f, kAccent, 0.95f, 0.8f); }

    void centered(const MenuLayout::Rect &r, const std::string &s, float size, Color c, float alpha, bool bold) {
        text(r.x + (r.w - measure(s, size, bold)) * 0.5f, r.y + r.h * 0.5f + size * 0.36f, s, size, c, alpha, bold);
    }

    void action_buttons(const std::vector<MenuLayout::Rect> &rects, const MenuItem &item, int focused, float alpha) {
        for (std::size_t a = 0; a < rects.size() && a < item.actions.size(); ++a) {
            const auto &r = rects[a];
            const bool primary = static_cast<int>(a) == item.primary_action;
            if (primary) rrect(r.x, r.y, r.w, r.h, 4.0f, kAccent, alpha);
            else rrect(r.x, r.y, r.w, r.h, 4.0f, kWhite, 0.16f * alpha, 0.6f);
            centered(r, item.actions[a], 5.2f, primary ? kInk : kText, alpha, true);
            if (static_cast<int>(a) == focused) ring(r, 4.0f);
        }
    }

    void row(const Placed &p, std::size_t i, const MenuItem &item, bool selected, const MenuView &v) {
        const auto &c = p.panels.content;
        const float x = c.x, w = c.w, y = p.row_y[i], h = row_height(item, w);
        const float alpha = item.enabled ? 1.0f : 0.4f;
        const bool keys = !v.pointer && !v.tabs_focused;
        const bool focus = selected && keys;       // the focus ring
        const bool hover = selected && v.pointer;  // the pointer is over it
        const MenuLayout::Rect card{x, y, w, h};
        switch (item.kind) {
        case MenuItem::Kind::Heading:
            text(x + 2.0f, y + 9.0f, upper(item.label), kSmall, kAccent, 0.95f, true, 0.6f);
            return;
        case MenuItem::Kind::Info: {
            rrect(x, y, w, h, kCard, kWhite, 0.04f);
            rrect(x + 7.0f, y + 6.2f, 4.2f, 4.2f, 2.1f, kBlue, 0.9f);
            float base = y + 9.4f;
            for (const std::string &line : wrap(item.label, 5.2f, w - 22.0f)) {
                text(x + 15.0f, base, line, 5.2f, kSoft, 0.95f);
                base += 7.2f;
            }
            return;
        }
        case MenuItem::Kind::Banner: {
            rrect(x, y, w, h, kCard, kAccent, 0.09f);
            rrect(x, y, w, h, kCard, kAccent, 0.3f, 0.5f);
            text(x + 8.0f, y + 10.0f, upper(item.label), kSmall, kWarm, 0.95f, true, 0.6f);
            text(x + 8.0f, y + 24.0f, item.text, 12.5f, kText, alpha, true, 0.8f);
            action_buttons(p.layout.actions[i], item, focus ? v.action : -1, alpha);
            return;
        }
        case MenuItem::Kind::Text: {
            text(x + 1.0f, y + 6.0f, item.label, 5.0f, kSoft, alpha, true);
            const auto &acts = p.layout.actions[i];
            const float right = acts.empty() ? x + w : acts.front().x - 3.5f;
            const MenuLayout::Rect field{x, y + 9.0f, right - x, 17.0f};
            const bool typing = item.value != 0;
            rrect(field.x, field.y, field.w, field.h, 4.0f, kBlack, 0.25f * alpha);
            rrect(field.x, field.y, field.w, field.h, 4.0f, typing ? kAccent : kWhite, typing ? 0.9f : 0.16f, typing ? 0.7f : 0.6f);
            const float base = field.y + 11.0f;
            if (item.text.empty() && !typing) {
                text(field.x + 6.0f, base, fit(item.placeholder, 5.4f, field.w - 12.0f), 5.4f, kDim, 0.9f);
            } else {
                const float tw = text(field.x + 6.0f, base, fit(item.text, 5.6f, field.w - 14.0f), 5.6f, kText, alpha, false, 0.4f);
                if (typing) rect(field.x + 6.5f + tw, field.y + 4.0f, 0.6f, 9.0f, kAccent, 1.0f); // the caret
            }
            if (focus && v.action < 0) ring(field, 4.0f);
            action_buttons(acts, item, focus ? v.action : -1, alpha);
            return;
        }
        case MenuItem::Kind::Choice:
            if (!item.option_details.empty()) {
                if (!item.label.empty()) text(x + 1.0f, y + 7.0f, item.label, 5.0f, kSoft, alpha, true);
                const auto &cards = p.layout.options[i];
                for (std::size_t o = 0; o < cards.size(); ++o) {
                    const auto &r = cards[o];
                    const bool on = static_cast<int>(o) == item.value;
                    rrect(r.x, r.y, r.w, r.h, 5.0f, on ? kAccent : kWhite, on ? 0.08f : 0.03f);
                    rrect(r.x, r.y, r.w, r.h, 5.0f, on ? kAccent : kWhite, on ? 0.95f : 0.1f, on ? 0.75f : 0.6f);
                    text(r.x + 7.0f, r.y + 11.0f, item.options[o], 6.0f, kText, alpha, true);
                    if (o < item.option_details.size()) {
                        float base = r.y + 18.5f;
                        for (const std::string &line : wrap(item.option_details[o], 4.8f, r.w - 14.0f)) {
                            text(r.x + 7.0f, base, line, 4.8f, kSoft, 0.9f);
                            base += 6.4f;
                        }
                    }
                    if (focus && on) ring(r, 5.0f);
                }
                return;
            }
            break;
        default: break;
        }

        // A card: the label on the left, the control on the right (Choice: the chips under the label).
        rrect(x, y, w, h, kCard, kWhite, hover ? 0.075f : 0.04f);
        if (focus && item.kind != MenuItem::Kind::Choice) ring(card, kCard);
        const Color label_color = item.danger ? kDanger : kText;
        const bool detail = !item.detail.empty() && item.kind != MenuItem::Kind::Choice;
        const float base = item.kind == MenuItem::Kind::Choice ? y + 9.5f : detail ? y + 10.5f : y + h * 0.5f + 2.1f;
        const float right = x + w - 8.0f;
        float control = 0.0f;
        switch (item.kind) {
        case MenuItem::Kind::Slider: {
            const auto &bar = p.layout.bars[i];
            const std::string number = std::to_string(item.value) + (item.max == 100 ? "%" : "");
            text(right - measure(number, 5.4f), y + h * 0.5f + 1.9f, number, 5.4f, kSoft, alpha);
            const float t = std::clamp(static_cast<float>(item.value - item.min) / static_cast<float>(std::max(item.max - item.min, 1)), 0.0f, 1.0f);
            const float by = bar.y + 2.2f;
            rrect(bar.x, by, bar.w, 1.6f, 0.8f, kWhite, 0.18f * alpha);
            if (t > 0.0f) rrect(bar.x, by, bar.w * t, 1.6f, 0.8f, kAccent, alpha);
            rrect(bar.x + bar.w * t - 3.0f, by - 2.2f, 6.0f, 6.0f, 3.0f, kWhite, alpha); // the thumb
            control = right - bar.x + 6.0f;
            break;
        }
        case MenuItem::Kind::Toggle: {
            const bool on = item.value != 0;
            const float pw = 16.5f, ph = 9.8f, px = right - pw, py = y + (h - ph) * 0.5f;
            rrect(px, py, pw, ph, ph * 0.5f, on ? kAccent : kWhite, on ? alpha : 0.18f * alpha);
            rrect(on ? px + pw - ph + 1.1f : px + 1.1f, py + 1.1f, ph - 2.2f, ph - 2.2f, (ph - 2.2f) * 0.5f, kWhite, alpha);
            control = pw + 8.0f;
            break;
        }
        case MenuItem::Kind::Binding: {
            const bool waiting = item.value != 0;
            const std::string shown = waiting ? "Press a key or button..." : (item.text.empty() ? "Not set" : item.text);
            const float cw = std::max(30.0f, measure(shown, 5.2f, true) + 10.0f), ch = 13.0f;
            const float cx = right - cw, cy = y + (h - ch) * 0.5f;
            rrect(cx, cy, cw, ch, 3.5f, waiting ? kAccent : kBlack, waiting ? 0.15f : 0.2f);
            rrect(cx, cy, cw, ch, 3.5f, waiting ? kAccent : kWhite, waiting ? 0.9f : 0.14f, 0.6f);
            centered({cx, cy, cw, ch}, shown, 5.2f, waiting ? kWarm : (item.text.empty() ? kDim : kText), alpha, true);
            control = cw + 8.0f;
            break;
        }
        case MenuItem::Kind::Button:
            if (!item.danger) text(right - 3.0f, y + h * 0.5f + 2.6f, "\xE2\x80\xBA", 8.0f, hover || focus ? kAccent : kDim, 1.0f, true);
            control = 10.0f;
            break;
        case MenuItem::Kind::Choice: {
            const auto &chips = p.layout.options[i];
            for (std::size_t o = 0; o < chips.size(); ++o) {
                const auto &r = chips[o];
                const bool on = static_cast<int>(o) == item.value;
                if (on) rrect(r.x, r.y, r.w, r.h, 3.5f, kAccent, alpha);
                else rrect(r.x, r.y, r.w, r.h, 3.5f, kWhite, 0.16f * alpha, 0.6f);
                centered(r, item.options[o], 5.2f, on ? kInk : kText, alpha, true);
                if (focus && on) ring(r, 3.5f);
            }
            break;
        }
        default: break;
        }
        const float lw = text(x + 8.0f, base, fit(item.label, kLabel, w - 16.0f - control), kLabel, label_color, alpha, true);
        if (item.kind == MenuItem::Kind::Choice && !item.detail.empty()) {
            text(x + 8.0f + lw + 5.0f, base, fit(item.detail, kDetail, w - 26.0f - lw), kDetail, kDim, 0.95f);
        } else if (detail) {
            text(x + 8.0f, y + 19.0f, fit(item.detail, kDetail, w - 16.0f - control), kDetail, kDim, 0.95f);
        }
    }

    void draw(const MenuView &v, bool backdrop) {
        if (backdrop) fill_all(kBlack, 0.45f);
        const Placed p = place(v);
        const auto &side = p.panels.side, &main = p.panels.main;

        // ---- the sidebar: the program and the game, the sections, the way back to the game
        rrect(side.x, side.y, side.w, side.h, kRadius, kPanel, 0.88f);
        rrect(side.x, side.y, side.w, side.h, kRadius, kWhite, 0.07f, 0.5f);
        image(side.x + 7.0f, side.y + 8.0f, 15.0f, kIcon, kIconSize);
        if (!v.title.empty()) text(side.x + 27.0f, side.y + 14.5f, fit(v.title, 5.8f, side.w - 32.0f), 5.8f, kText, 1.0f, true);
        if (!v.subtitle.empty()) text(side.x + 27.0f, side.y + 21.0f, fit(v.subtitle, 4.5f, side.w - 32.0f), 4.5f, kDim, 1.0f);
        for (std::size_t i = 0; i < v.tabs.size(); ++i) {
            const auto &r = p.layout.tabs[i];
            const bool active = static_cast<int>(i) == v.tab;
            if (active) rrect(r.x, r.y, r.w, r.h, 4.0f, kAccent, 0.14f);
            text(r.x + 7.0f, r.y + r.h * 0.5f + 2.1f, v.tabs[i].name, kLabel, active ? kWarm : kSoft, 1.0f, true);
            if (!v.tabs[i].badge.empty()) {
                const bool private_badge = v.tabs[i].badge != "Public";
                const float bw = measure(v.tabs[i].badge, 4.2f, true) + 6.0f;
                const MenuLayout::Rect b{r.x + r.w - bw - 4.0f, r.y + (r.h - 8.0f) * 0.5f, bw, 8.0f};
                rrect(b.x, b.y, b.w, b.h, 4.0f, private_badge ? kAccent : kGreen, 0.2f);
                centered(b, v.tabs[i].badge, 4.2f, private_badge ? kWarm : kGreen, 1.0f, true);
            }
            if (active && v.tabs_focused && !v.pointer) ring(r, 4.0f);
        }
        const auto &resume = p.layout.resume;
        rrect(resume.x, resume.y, resume.w, resume.h, 4.5f, kAccent, 1.0f);
        centered(resume, v.resume, 5.6f, kInk, 1.0f, true);
        const std::string esc = "Esc also closes this menu";
        text(side.x + (side.w - measure(esc, 4.2f)) * 0.5f, side.y + side.h - 5.5f, esc, 4.2f, kDim, 0.9f);

        // ---- the panel: the section's title, its rows, the key hints
        rrect(main.x, main.y, main.w, main.h, kRadius, kPanel, 0.88f);
        rrect(main.x, main.y, main.w, main.h, kRadius, kWhite, 0.07f, 0.5f);
        centered(p.layout.close, "\xC3\x97", 10.0f, kDim, 1.0f, false);
        if (v.tab >= 0 && v.tab < static_cast<int>(v.tabs.size())) {
            const MenuTab &tab = v.tabs[static_cast<std::size_t>(v.tab)];
            text(main.x + 14.0f, main.y + 19.0f, tab.name, 9.5f, kText, 1.0f, true);
            if (!tab.subtitle.empty()) text(main.x + 14.0f, main.y + 27.5f, fit(tab.subtitle, 5.2f, main.w - 46.0f), 5.2f, kDim, 1.0f);
        }
        const auto &rows = v.items();
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (!p.shown[i]) continue;
            row(p, i, rows[i], static_cast<int>(i) == v.selected, v);
        }
        if (p.layout.keypad_row >= 0) keypad(p, rows[static_cast<std::size_t>(p.layout.keypad_row)], v);
        const auto &c = p.panels.content;
        if (p.total > c.h + 0.5f) { // more rows than room: a thin scroll bar
            const float thumb = std::max(12.0f, c.h * c.h / p.total);
            const float at = p.scroll / std::max(p.total - c.h, 1.0f) * (c.h - thumb);
            rrect(main.x + main.w - 6.0f, c.y + at, 1.4f, thumb, 0.7f, kWhite, 0.3f);
        }
        if (!v.pointer && !v.hint.empty()) {
            const float hw = measure(v.hint, 4.6f, false, 0.3f);
            text(main.x + (main.w - hw) * 0.5f, main.y + main.h - 6.0f, v.hint, 4.6f, kDim, 0.9f, false, 0.3f);
        }
    }
};

MenuRenderer::MenuRenderer(const std::filesystem::path &font_directory) : impl_(std::make_unique<Impl>(font_directory)) {}
MenuRenderer::~MenuRenderer() = default;

MenuLayout MenuRenderer::layout(const MenuView &view) const { return impl_->place(view).layout; }

bool MenuRenderer::render(const MenuView &view, int scale, std::vector<std::uint32_t> &pixels, bool backdrop) {
    return render(view, width(scale), height(scale), pixels, backdrop);
}

bool MenuRenderer::render(const MenuView &view, int width, int height, std::vector<std::uint32_t> &pixels, bool backdrop) {
    Impl &d = *impl_;
    d.begin(pixels, width, height, static_cast<float>(height) / Impl::kH);
    d.draw(view, backdrop);
    d.end();
    return d.font_ok();
}

} // namespace mhf::overlay
