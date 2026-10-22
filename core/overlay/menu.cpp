#include "overlay/menu.hpp"

#include "overlay/icon.hpp"
#include "overlay/painter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace mhf::overlay {

// ---------------------------------------------------------------- navigation

bool Menu::selectable(const MenuItem &item) {
    return item.enabled && item.kind != MenuItem::Kind::Info && item.kind != MenuItem::Kind::Heading;
}

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
    int keep_id = -1;
    bool had_row = false;
    if (view_.tab >= 0 && view_.tab < static_cast<int>(view_.tabs.size())) {
        keep_tab = view_.tabs[static_cast<std::size_t>(view_.tab)].name;
        const auto &rows = view_.items();
        if (view_.selected >= 0 && view_.selected < static_cast<int>(rows.size())) {
            keep_id = rows[static_cast<std::size_t>(view_.selected)].id;
            had_row = true;
        }
    }
    const bool focus = view_.tabs_focused;
    view_ = std::move(view);
    view_.version = ++version_;
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
}

void Menu::move(int direction) {
    const auto &rows = view_.items();
    const int count = static_cast<int>(rows.size());
    for (int step = 1; step <= count; ++step) {
        const int i = ((view_.selected + direction * step) % count + count) % count;
        if (selectable(rows[static_cast<std::size_t>(i)])) {
            view_.selected = i;
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
    view_.version = ++version_;
}

Menu::Event Menu::press(Key key) {
    if (key == Key::Back) return {Event::Kind::Back, 0, 0};
    if (view_.tabs.empty()) return {};
    if (key == Key::PrevTab || key == Key::NextTab) {
        change_tab(key == Key::PrevTab ? -1 : 1);
        return {};
    }
    if (view_.tabs_focused) { // the tab bar: Left/Right pick the tab, Down (or Accept) goes into it
        if (key == Key::Left || key == Key::Right) change_tab(key == Key::Left ? -1 : 1);
        if (key == Key::Down || key == Key::Accept) {
            view_.tabs_focused = false;
            view_.selected = first_selectable();
            view_.version = ++version_;
        }
        return {};
    }
    auto &rows = items();
    if (rows.empty()) {
        if (key == Key::Up && view_.tabs.size() > 1) {
            view_.tabs_focused = true;
            view_.version = ++version_;
        }
        return {};
    }
    if (key == Key::Up || key == Key::Down) {
        // Up from the first row goes to the tab bar.
        if (key == Key::Up && view_.tabs.size() > 1 && view_.selected <= first_selectable()) {
            view_.tabs_focused = true;
            view_.version = ++version_;
            return {};
        }
        move(key == Key::Up ? -1 : 1);
        return {};
    }
    if (view_.selected < 0 || view_.selected >= static_cast<int>(rows.size())) return {};
    MenuItem &item = rows[static_cast<std::size_t>(view_.selected)];
    if (!selectable(item)) return {};
    const auto changed = [&](int value) { return set_value(item, value); };
    const int direction = key == Key::Left ? -1 : key == Key::Right ? 1 : 0;
    switch (item.kind) {
    case MenuItem::Kind::Button:
        if (key == Key::Accept) return {Event::Kind::Activated, item.id, 0};
        return {};
    case MenuItem::Kind::Slider:
        if (direction != 0) return changed(std::clamp(item.value + direction * std::max(item.step, 1), item.min, item.max));
        return {};
    case MenuItem::Kind::Toggle:
        if (key == Key::Accept || direction != 0) return changed(item.value != 0 ? 0 : 1);
        return {};
    case MenuItem::Kind::Choice: {
        const int n = static_cast<int>(item.options.size());
        if (n == 0) return {};
        if (key == Key::Accept) return changed((item.value + 1) % n);
        return changed(std::clamp(item.value + direction, 0, n - 1));
    }
    default: return {};
    }
}

// ---------------------------------------------------------------- the mouse

MenuLayout::Hit MenuLayout::hit(float x, float y) const {
    Hit h;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        if (tabs[i].contains(x, y)) {
            h.kind = Hit::Kind::Tab;
            h.index = static_cast<int>(i);
            return h;
        }
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].contains(x, y)) continue;
        h.kind = Hit::Kind::Row;
        h.index = static_cast<int>(i);
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

void Menu::hover_row(int row) {
    if (view_.tabs.empty() || row < 0 || row >= static_cast<int>(view_.items().size())) return;
    if (!selectable(view_.items()[static_cast<std::size_t>(row)])) return;
    if (view_.selected != row || view_.tabs_focused) {
        view_.selected = row;
        view_.tabs_focused = false;
        view_.version = ++version_;
    }
}

void Menu::click_tab(int tab) {
    if (tab < 0 || tab >= static_cast<int>(view_.tabs.size()) || tab == view_.tab) return;
    view_.tab = tab;
    view_.selected = first_selectable();
    view_.tabs_focused = false;
    view_.version = ++version_;
}

Menu::Event Menu::click_row(int row, float along, bool on_bar) {
    if (view_.tabs.empty() || row < 0 || row >= static_cast<int>(items().size())) return {};
    hover_row(row);
    MenuItem &item = items()[static_cast<std::size_t>(row)];
    if (!selectable(item)) return {};
    switch (item.kind) {
    case MenuItem::Kind::Button: return {Event::Kind::Activated, item.id, 0};
    case MenuItem::Kind::Toggle: return set_value(item, item.value != 0 ? 0 : 1);
    case MenuItem::Kind::Choice: {
        const int n = static_cast<int>(item.options.size());
        if (n == 0) return {};
        return set_value(item, std::clamp(item.value + (along < 0.5f ? -1 : 1), 0, n - 1));
    }
    case MenuItem::Kind::Slider:
        if (on_bar) return drag_slider(row, along);
        return {};
    default: return {};
    }
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
    change_tab(-direction); // the wheel up goes to the previous tab
    return {};
}

// ---------------------------------------------------------------- drawing

namespace {

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

struct MenuRenderer::Impl : Painter {
    explicit Impl(const std::filesystem::path &font_directory) : Painter(font_directory) {}

    // Logical units of the 480x272 canvas.
    static constexpr float kW = 480.0f, kH = 272.0f;
    static constexpr float kMargin = 26.0f;  // left/right of the top bar and the hairline
    static constexpr float kBarY = 28.0f;    // baseline of the tab labels
    static constexpr float kLineY = 40.0f;   // hairline under the tabs
    static constexpr float kBlockW = 286.0f; // the rows, centred
    static constexpr float kAreaTop = 52.0f, kAreaBottom = 244.0f;
    static constexpr float kTabSize = 8.0f, kTabTrack = 1.5f, kTabGap = 30.0f;
    static constexpr float kBarW = 92.0f;    // a slider's track

    static float row_height(const MenuItem &item) {
        switch (item.kind) {
        case MenuItem::Kind::Heading: return 22.0f;
        case MenuItem::Kind::Info: return 14.0f;
        default: return item.detail.empty() ? 23.0f : 31.0f;
        }
    }

    struct Placed {
        MenuLayout layout;
        std::vector<float> tab_x, tab_w; // the labels
        float scroll = 0.0f, top = 0.0f, content = 0.0f, total_rows = 0.0f;
    };

    Placed place(const MenuView &v) const {
        Placed p;
        float total = 0.0f;
        for (const MenuTab &t : v.tabs) {
            p.tab_w.push_back(measure(upper(t.name), kTabSize, true, kTabTrack) - kTabTrack);
            total += p.tab_w.back();
        }
        if (!v.tabs.empty()) total += kTabGap * static_cast<float>(v.tabs.size() - 1);
        float tx = (kW - total) * 0.5f;
        for (const float w : p.tab_w) {
            p.tab_x.push_back(tx);
            p.layout.tabs.push_back({tx - 12.0f, kBarY - 13.0f, w + 24.0f, 24.0f});
            tx += w + kTabGap;
        }

        const auto &rows = v.items();
        std::vector<float> offsets(rows.size() + 1, 0.0f);
        for (std::size_t i = 0; i < rows.size(); ++i) offsets[i + 1] = offsets[i] + row_height(rows[i]);
        const float room = kAreaBottom - kAreaTop;
        p.total_rows = offsets.back();
        p.content = std::min(offsets.back(), room);
        p.top = kAreaTop + std::max(0.0f, (room - p.content) * 0.5f - 6.0f);
        if (v.selected >= 0 && v.selected < static_cast<int>(rows.size())) {
            const auto s = static_cast<std::size_t>(v.selected);
            if (offsets[s + 1] > p.content) p.scroll = offsets[s + 1] - p.content;
            if (s > 0 && rows[s - 1].kind == MenuItem::Kind::Heading && offsets[s - 1] < p.scroll) p.scroll = offsets[s - 1];
        }
        const float x = (kW - kBlockW) * 0.5f;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const float h = row_height(rows[i]);
            const float y = p.top + offsets[i] - p.scroll;
            const bool visible = y >= p.top - 0.5f && y + h <= p.top + p.content + 0.5f;
            const bool pickable = visible && rows[i].kind != MenuItem::Kind::Heading && rows[i].kind != MenuItem::Kind::Info;
            p.layout.rows.push_back(pickable ? MenuLayout::Rect{x, y, kBlockW, h - 1.0f} : MenuLayout::Rect{});
            MenuLayout::Rect bar;
            if (pickable && rows[i].kind == MenuItem::Kind::Slider) {
                const float base = rows[i].detail.empty() ? y + (h - 1.0f) * 0.5f + 3.2f : y + 12.5f;
                bar = {x + kBlockW - 12.0f - 24.0f - kBarW, base - 3.0f, kBarW, 1.6f};
            }
            p.layout.bars.push_back(bar);
        }
        return p;
    }

    void row(float x, float y, float w, const MenuItem &item, bool selected, bool focused) {
        const float h = row_height(item);
        if (item.kind == MenuItem::Kind::Heading) {
            text(x + 12.0f, y + 15.5f, upper(item.label), 6.4f, kAccent, 0.9f, true, 1.0f);
            return;
        }
        if (item.kind == MenuItem::Kind::Info) {
            text(x + 12.0f, y + 9.5f, fit(item.label, 7.0f, w - 24.0f), 7.0f, kDim, 0.9f);
            return;
        }
        const float alpha = item.enabled ? 1.0f : 0.4f;
        const bool lit = selected && focused;
        if (lit) {
            rect(x, y, w, h - 1.0f, kAccent, 0.10f);
            rect(x, y, 2.0f, h - 1.0f, kAccent, 1.0f);
        }
        rect(x + 8.0f, y + h - 1.0f, w - 16.0f, 0.5f, kWhite, 0.06f); // hairline between rows
        const Color label_color = item.danger ? kDanger : (lit ? kText : Color{0.82f, 0.84f, 0.89f});
        const float base = item.detail.empty() ? y + (h - 1.0f) * 0.5f + 3.2f : y + 12.5f;
        const float right = x + w - 12.0f;
        float control = 0.0f; // room taken by the control on the right
        switch (item.kind) {
        case MenuItem::Kind::Slider: {
            const std::string number = std::to_string(item.value);
            text(right - measure(number, 8.0f), base, number, 8.0f, lit ? kText : kDim, alpha);
            const float bx = right - 24.0f - kBarW, by = base - 3.0f;
            const float span = static_cast<float>(std::max(item.max - item.min, 1));
            const float t = std::clamp(static_cast<float>(item.value - item.min) / span, 0.0f, 1.0f);
            rect(bx, by, kBarW, 1.6f, kWhite, 0.18f * alpha);
            if (t > 0.0f) rect(bx, by, kBarW * t, 1.6f, kAccent, alpha);
            rect(bx + kBarW * t - 1.0f, by - 2.6f, 2.0f, 6.8f, lit ? kText : kDim, alpha); // the thumb
            control = 24.0f + kBarW + 10.0f;
            break;
        }
        case MenuItem::Kind::Toggle: {
            const bool on = item.value != 0;
            const float pw = 22.0f, ph = 9.0f, px = right - pw, py = base - 7.0f;
            rrect(px, py, pw, ph, 1.6f, on ? kAccent : kWhite, on ? alpha : 0.16f * alpha);
            const float kw = 8.0f;
            rrect(on ? px + pw - kw - 1.0f : px + 1.0f, py + 1.0f, kw, ph - 2.0f, 1.0f, on ? kInk : kDim, alpha);
            control = pw + 10.0f;
            break;
        }
        case MenuItem::Kind::Choice: {
            const std::string shown =
                item.value >= 0 && item.value < static_cast<int>(item.options.size()) ? item.options[static_cast<std::size_t>(item.value)] : "";
            const float tw = measure(shown, 8.0f, true);
            text(right - tw, base, shown, 8.0f, kText, alpha, true);
            if (lit) {
                if (item.value > 0) text(right - tw - 9.0f, base, "\xE2\x80\xB9", 11.0f, kAccent, 1.0f, true);
                if (item.value + 1 < static_cast<int>(item.options.size())) text(right + 3.0f, base, "\xE2\x80\xBA", 11.0f, kAccent, 1.0f, true);
            }
            control = tw + 22.0f;
            break;
        }
        case MenuItem::Kind::Button:
            if (lit) text(right - 4.0f, base, "\xE2\x80\xBA", 11.0f, kAccent, 1.0f, true);
            control = 14.0f;
            break;
        default: break;
        }
        text(x + 12.0f, base, fit(item.label, 9.0f, w - 24.0f - control), 9.0f, label_color, alpha, lit);
        if (!item.detail.empty()) text(x + 12.0f, y + 23.5f, fit(item.detail, 6.6f, w - 24.0f), 6.6f, kDim, alpha);
    }

    void draw(const MenuView &v, bool backdrop) {
        if (backdrop) fill_all(kBlack, 0.45f);
        const Placed p = place(v);

        // ---- top bar: the project's icon on the left, the tabs in the middle, what Escape does on the right
        image(kMargin, kBarY - 13.0f, 16.0f, kIcon, kIconSize);
        if (!v.corner.empty()) {
            const float cw = measure(upper(v.corner), 6.4f, false, 0.9f);
            text(kW - kMargin - cw, kBarY, upper(v.corner), 6.4f, kDim, 0.8f, false, 0.9f);
        }
        for (std::size_t i = 0; i < v.tabs.size(); ++i) {
            const bool active = static_cast<int>(i) == v.tab;
            const float tx = p.tab_x[i];
            if (active && v.tabs_focused) rect(tx - 9.0f, kBarY - 11.0f, p.tab_w[i] + 18.0f, 17.0f, kAccent, 0.12f);
            text(tx, kBarY, upper(v.tabs[i].name), kTabSize, active ? kText : kDim, active ? 1.0f : 0.75f, true, kTabTrack);
            if (active) rect(tx, kBarY + 6.0f, p.tab_w[i], 1.6f, kAccent, 1.0f);
        }
        rect(kMargin, kLineY, kW - 2.0f * kMargin, 0.5f, kWhite, 0.12f);

        // ---- rows, centred in the area below
        const auto &rows = v.items();
        const float x = (kW - kBlockW) * 0.5f;
        float offset = 0.0f;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const float h = row_height(rows[i]);
            const float y = p.top + offset - p.scroll;
            offset += h;
            if (y < p.top - 0.5f || y + h > p.top + p.content + 0.5f) continue;
            row(x, y, kBlockW, rows[i], static_cast<int>(i) == v.selected, !v.tabs_focused);
        }
        if (p.total_rows > p.content) { // more rows than room: a thin scroll bar
            const float thumb = std::max(12.0f, p.content * p.content / p.total_rows);
            const float at = p.scroll / std::max(p.total_rows - p.content, 1.0f) * (p.content - thumb);
            rect(x + kBlockW + 6.0f, p.top + at, 1.2f, thumb, kWhite, 0.3f);
        }

        // ---- the hint line
        if (!v.hint.empty()) {
            const float size = 6.4f, track = 0.6f;
            const float hw = measure(v.hint, size, false, track);
            text((kW - hw) * 0.5f, kH - 18.0f, v.hint, size, kDim, 0.75f, false, track);
        }
    }
};

MenuRenderer::MenuRenderer(const std::filesystem::path &font_directory) : impl_(std::make_unique<Impl>(font_directory)) {}
MenuRenderer::~MenuRenderer() = default;

MenuLayout MenuRenderer::layout(const MenuView &view) const { return impl_->place(view).layout; }

bool MenuRenderer::render(const MenuView &view, int scale, std::vector<std::uint32_t> &pixels, bool backdrop) {
    Impl &d = *impl_;
    d.begin(pixels, width(scale), height(scale), static_cast<float>(scale));
    d.draw(view, backdrop);
    d.end();
    return d.font_ok();
}

} // namespace mhf::overlay
