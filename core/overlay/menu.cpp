#include "overlay/menu.hpp"

#include "overlay/painter.hpp"

#include <algorithm>
#include <cmath>

namespace mhf::overlay {

// ---------------------------------------------------------------- navigation

bool Menu::selectable(const MenuItem &item) {
    return item.enabled && item.kind != MenuItem::Kind::Info && item.kind != MenuItem::Kind::Heading;
}

void Menu::set(MenuView view) {
    int keep_id = -1;
    bool had = false;
    if (view_.selected >= 0 && view_.selected < static_cast<int>(view_.items.size())) {
        keep_id = view_.items[static_cast<std::size_t>(view_.selected)].id;
        had = true;
    }
    view_ = std::move(view);
    view_.version = ++version_;
    int found = -1;
    for (std::size_t i = 0; i < view_.items.size(); ++i) {
        if (had && view_.items[i].id == keep_id && selectable(view_.items[i])) {
            found = static_cast<int>(i);
            break;
        }
    }
    if (found < 0 && view_.selected >= 0 && view_.selected < static_cast<int>(view_.items.size()) &&
        selectable(view_.items[static_cast<std::size_t>(view_.selected)])) {
        found = view_.selected; // the owner chose one
    }
    if (found < 0) {
        for (std::size_t i = 0; i < view_.items.size() && found < 0; ++i) {
            if (selectable(view_.items[i])) found = static_cast<int>(i);
        }
    }
    view_.selected = std::max(found, 0);
}

void Menu::move(int direction) {
    const int count = static_cast<int>(view_.items.size());
    for (int step = 1; step <= count; ++step) {
        const int i = ((view_.selected + direction * step) % count + count) % count;
        if (selectable(view_.items[static_cast<std::size_t>(i)])) {
            view_.selected = i;
            view_.version = ++version_;
            return;
        }
    }
}

Menu::Event Menu::press(Key key) {
    if (key == Key::Back) return {Event::Kind::Back, 0, 0};
    if (view_.items.empty()) return {};
    if (key == Key::Up || key == Key::Down) {
        move(key == Key::Up ? -1 : 1);
        return {};
    }
    if (view_.selected < 0 || view_.selected >= static_cast<int>(view_.items.size())) return {};
    MenuItem &item = view_.items[static_cast<std::size_t>(view_.selected)];
    if (!selectable(item)) return {};
    const auto changed = [&](int value) -> Event {
        if (value == item.value) return {};
        item.value = value;
        view_.version = ++version_;
        return {Event::Kind::Changed, item.id, value};
    };
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

// ---------------------------------------------------------------- drawing

struct MenuRenderer::Impl : Painter {
    explicit Impl(const std::filesystem::path &font_directory) : Painter(font_directory) {}

    static constexpr float kPanelX = 20.0f, kPanelY = 14.0f, kPanelW = 240.0f, kPanelH = 244.0f, kPad = 14.0f;

    static float row_height(const MenuItem &item) {
        switch (item.kind) {
        case MenuItem::Kind::Heading: return 20.0f;
        case MenuItem::Kind::Info: return 15.0f;
        default: return item.detail.empty() ? 22.0f : 30.0f;
        }
    }

    void panel() {
        rrect(kPanelX, kPanelY + 3.0f, kPanelW, kPanelH, 12.0f, kBlack, 0.35f); // shadow
        rrect(kPanelX, kPanelY, kPanelW, kPanelH, 12.0f, kPanelTop, 0.97f, 0.0f, &kPanelBottom);
        rrect(kPanelX, kPanelY, kPanelW, kPanelH, 12.0f, kWhite, 0.09f, 0.6f);
        rrect(kPanelX + 18.0f, kPanelY, kPanelW - 36.0f, 1.6f, 0.8f, kAccent, 0.95f); // accent line
    }

    void row(float x, float y, float w, const MenuItem &item, bool selected) {
        const float h = row_height(item);
        if (item.kind == MenuItem::Kind::Heading) {
            text(x + 2.0f, y + 14.0f, item.label, 8.5f, kAccent, 0.95f, true);
            return;
        }
        if (item.kind == MenuItem::Kind::Info) {
            text(x + 2.0f, y + 11.0f, fit(item.label, 8.5f, w - 4.0f), 8.5f, kDim);
            return;
        }
        const float alpha = item.enabled ? 1.0f : 0.4f;
        if (selected) {
            rrect(x, y, w, h - 2.0f, 7.0f, kAccent, 0.13f);
            rrect(x, y, w, h - 2.0f, 7.0f, kAccent, 0.85f, 0.8f);
        } else {
            rrect(x, y, w, h - 2.0f, 7.0f, kWhite, 0.035f);
        }
        const Color label_color = item.danger ? kDanger : kText;
        const float base = item.detail.empty() ? y + (h - 2.0f) * 0.5f + 3.6f : y + 12.0f;
        // The control on the right first, to know how much room the label has.
        float control = 0.0f;
        const float right = x + w - 10.0f;
        switch (item.kind) {
        case MenuItem::Kind::Slider: {
            const std::string number = std::to_string(item.value);
            const float nw = 22.0f;
            text(right - measure(number, 9.0f), base, number, 9.0f, kText, alpha);
            const float bar_w = 64.0f, bx = right - nw - bar_w - 4.0f, by = base - 5.0f;
            const float span = static_cast<float>(std::max(item.max - item.min, 1));
            const float t = std::clamp(static_cast<float>(item.value - item.min) / span, 0.0f, 1.0f);
            rrect(bx, by, bar_w, 4.0f, 2.0f, kWhite, 0.14f * alpha);
            if (t > 0.0f) rrect(bx, by, std::max(bar_w * t, 4.0f), 4.0f, 2.0f, kAccent, alpha);
            control = nw + bar_w + 8.0f;
            break;
        }
        case MenuItem::Kind::Toggle: {
            const bool on = item.value != 0;
            const float pw = 24.0f, ph = 12.0f, px = right - pw, py = base - 8.5f;
            rrect(px, py, pw, ph, ph * 0.5f, on ? kAccent : kWhite, on ? alpha : 0.14f * alpha);
            rrect(on ? px + pw - ph + 1.5f : px + 1.5f, py + 1.5f, ph - 3.0f, ph - 3.0f, (ph - 3.0f) * 0.5f, on ? kInk : kDim, alpha);
            control = pw + 6.0f;
            break;
        }
        case MenuItem::Kind::Choice: {
            const std::string shown =
                item.value >= 0 && item.value < static_cast<int>(item.options.size()) ? item.options[static_cast<std::size_t>(item.value)] : "";
            const float tw = measure(shown, 9.0f, true);
            text(right - tw, base, shown, 9.0f, kText, alpha, true);
            if (selected) {
                text(right - tw - 9.0f, base, "\xE2\x80\xB9", 11.0f, kAccent, alpha, true);
                text(right + 2.0f, base, "\xE2\x80\xBA", 11.0f, kAccent, alpha, true);
            }
            control = tw + 16.0f;
            break;
        }
        default: break;
        }
        text(x + 10.0f, base, fit(item.label, 10.0f, w - 20.0f - control), 10.0f, label_color, alpha, selected);
        if (!item.detail.empty()) text(x + 10.0f, y + 23.0f, fit(item.detail, 7.5f, w - 20.0f), 7.5f, kDim, alpha);
    }

    void draw(const MenuView &v) {
        fill_all(kBlack, 0.55f);
        panel();
        const float x = kPanelX + kPad, w = kPanelW - kPad * 2.0f;
        text(x, kPanelY + 28.0f, fit(v.title, 13.0f, w), 13.0f, kText, 1.0f, true);
        if (!v.subtitle.empty()) text(x, kPanelY + 40.0f, fit(v.subtitle, 8.5f, w), 8.5f, kDim);

        // Rows, scrolled so that the selected one is in view.
        const float top = kPanelY + 50.0f, bottom = kPanelY + kPanelH - (v.hint.empty() ? 12.0f : 24.0f);
        const float room = bottom - top;
        std::vector<float> offsets(v.items.size() + 1, 0.0f);
        for (std::size_t i = 0; i < v.items.size(); ++i) offsets[i + 1] = offsets[i] + row_height(v.items[i]);
        float scroll = 0.0f;
        if (v.selected >= 0 && v.selected < static_cast<int>(v.items.size())) {
            const auto s = static_cast<std::size_t>(v.selected);
            if (offsets[s + 1] > room) scroll = offsets[s + 1] - room;
            // A heading right above the selection stays with it.
            if (s > 0 && v.items[s - 1].kind == MenuItem::Kind::Heading && offsets[s - 1] < scroll) scroll = offsets[s - 1];
        }
        for (std::size_t i = 0; i < v.items.size(); ++i) {
            const float y = top + offsets[i] - scroll;
            if (y < top - 0.5f || y + row_height(v.items[i]) > bottom + 0.5f) continue;
            row(x, y, w, v.items[i], static_cast<int>(i) == v.selected);
        }
        if (offsets.back() > room) { // more rows than room: a thin scroll bar
            const float track = room, thumb = std::max(12.0f, track * room / offsets.back());
            const float at = scroll / std::max(offsets.back() - room, 1.0f) * (track - thumb);
            rrect(kPanelX + kPanelW - 7.0f, top + at, 2.0f, thumb, 1.0f, kWhite, 0.3f);
        }
        if (!v.hint.empty()) {
            const float size = 7.5f;
            text(kPanelX + (kPanelW - measure(v.hint, size)) * 0.5f, kPanelY + kPanelH - 9.0f, fit(v.hint, size, w), size, kDim, 0.9f);
        }
    }
};

MenuRenderer::MenuRenderer(const std::filesystem::path &font_directory) : impl_(std::make_unique<Impl>(font_directory)) {}
MenuRenderer::~MenuRenderer() = default;

bool MenuRenderer::render(const MenuView &view, int scale, std::vector<std::uint32_t> &pixels) {
    Impl &d = *impl_;
    d.begin(pixels, width(scale), height(scale), static_cast<float>(scale));
    d.draw(view);
    d.end();
    return d.font_ok();
}

} // namespace mhf::overlay
