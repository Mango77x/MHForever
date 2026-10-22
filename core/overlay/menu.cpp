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

    // A compact panel on the left that is as tall as its rows (logical units of the 480x272 canvas).
    static constexpr float kPanelX = 14.0f, kPanelY = 14.0f, kPanelW = 168.0f, kPad = 10.0f;
    static constexpr float kHeaderH = 31.0f, kFooterH = 18.0f, kMaxH = 244.0f;

    static float row_height(const MenuItem &item) {
        switch (item.kind) {
        case MenuItem::Kind::Heading: return 15.0f;
        case MenuItem::Kind::Info: return 10.5f;
        default: return item.detail.empty() ? 15.5f : 23.0f;
        }
    }

    void panel(float h) {
        rrect(kPanelX, kPanelY + 2.0f, kPanelW, h, 9.0f, kBlack, 0.30f); // shadow
        rrect(kPanelX, kPanelY, kPanelW, h, 9.0f, kPanelTop, 0.95f, 0.0f, &kPanelBottom);
        rrect(kPanelX, kPanelY, kPanelW, h, 9.0f, kWhite, 0.08f, 0.5f);
    }

    void row(float x, float y, float w, const MenuItem &item, bool selected) {
        const float h = row_height(item);
        if (item.kind == MenuItem::Kind::Heading) {
            text(x + 3.0f, y + 11.0f, item.label, 6.2f, kDim, 0.85f, true);
            return;
        }
        if (item.kind == MenuItem::Kind::Info) {
            text(x + 3.0f, y + 7.5f, fit(item.label, 6.6f, w - 6.0f), 6.6f, kDim, 0.9f);
            return;
        }
        const float alpha = item.enabled ? 1.0f : 0.4f;
        const float rh = h - 2.0f;
        if (selected) {
            rrect(x, y, w, rh, 5.0f, kAccent, 0.14f);
            rrect(x, y, 2.0f, rh, 1.0f, kAccent, 0.95f); // accent bar on the left
        }
        const Color label_color = item.danger ? kDanger : kText;
        const float base = item.detail.empty() ? y + rh * 0.5f + 3.0f : y + 9.5f;
        const float right = x + w - 7.0f;
        float control = 0.0f; // room taken by the control on the right
        switch (item.kind) {
        case MenuItem::Kind::Slider: {
            const std::string number = std::to_string(item.value);
            text(right - measure(number, 7.5f), base, number, 7.5f, kDim, alpha);
            const float bar_w = 42.0f, bx = right - 16.0f - bar_w, by = base - 3.6f;
            const float span = static_cast<float>(std::max(item.max - item.min, 1));
            const float t = std::clamp(static_cast<float>(item.value - item.min) / span, 0.0f, 1.0f);
            rrect(bx, by, bar_w, 2.4f, 1.2f, kWhite, 0.16f * alpha);
            if (t > 0.0f) rrect(bx, by, std::max(bar_w * t, 2.4f), 2.4f, 1.2f, kAccent, alpha);
            if (selected) rrect(bx + bar_w * t - 2.4f, by - 1.2f, 4.8f, 4.8f, 2.4f, kText, alpha); // knob
            control = 16.0f + bar_w + 8.0f;
            break;
        }
        case MenuItem::Kind::Toggle: {
            const bool on = item.value != 0;
            const float pw = 19.0f, ph = 9.5f, px = right - pw, py = base - 7.0f;
            rrect(px, py, pw, ph, ph * 0.5f, on ? kAccent : kWhite, on ? alpha : 0.16f * alpha);
            const float kr = ph - 3.0f;
            rrect(on ? px + pw - kr - 1.5f : px + 1.5f, py + 1.5f, kr, kr, kr * 0.5f, on ? kInk : kDim, alpha);
            control = pw + 6.0f;
            break;
        }
        case MenuItem::Kind::Choice: {
            const std::string shown =
                item.value >= 0 && item.value < static_cast<int>(item.options.size()) ? item.options[static_cast<std::size_t>(item.value)] : "";
            const float tw = measure(shown, 7.5f, true);
            text(right - tw, base, shown, 7.5f, kText, alpha, true);
            control = tw + 8.0f;
            break;
        }
        default: break;
        }
        text(x + 8.0f, base, fit(item.label, 8.0f, w - 16.0f - control), 8.0f, label_color, alpha, false);
        if (!item.detail.empty()) text(x + 8.0f, y + 19.0f, fit(item.detail, 6.5f, w - 16.0f), 6.5f, kDim, alpha);
    }

    void draw(const MenuView &v) {
        fill_all(kBlack, 0.35f);
        // Row positions first: the panel is as tall as they are.
        std::vector<float> offsets(v.items.size() + 1, 0.0f);
        for (std::size_t i = 0; i < v.items.size(); ++i) offsets[i + 1] = offsets[i] + row_height(v.items[i]);
        const float footer = v.hint.empty() ? 8.0f : kFooterH;
        const float content = std::min(offsets.back(), kMaxH - kHeaderH - footer);
        const float h = kHeaderH + content + footer;
        panel(h);

        const float x = kPanelX + kPad, w = kPanelW - kPad * 2.0f;
        text(x, kPanelY + 17.0f, fit(v.title, 10.5f, w), 10.5f, kText, 1.0f, true);
        if (!v.subtitle.empty()) text(x, kPanelY + 26.0f, fit(v.subtitle, 6.6f, w), 6.6f, kDim);

        // Rows, scrolled so that the selected one is in view.
        const float top = kPanelY + kHeaderH;
        float scroll = 0.0f;
        if (v.selected >= 0 && v.selected < static_cast<int>(v.items.size())) {
            const auto s = static_cast<std::size_t>(v.selected);
            if (offsets[s + 1] > content) scroll = offsets[s + 1] - content;
            // A heading right above the selection stays with it.
            if (s > 0 && v.items[s - 1].kind == MenuItem::Kind::Heading && offsets[s - 1] < scroll) scroll = offsets[s - 1];
        }
        for (std::size_t i = 0; i < v.items.size(); ++i) {
            const float y = top + offsets[i] - scroll;
            if (y < top - 0.5f || y + row_height(v.items[i]) > top + content + 0.5f) continue;
            row(x, y, w, v.items[i], static_cast<int>(i) == v.selected);
        }
        if (offsets.back() > content) { // more rows than room: a thin scroll bar
            const float thumb = std::max(10.0f, content * content / offsets.back());
            const float at = scroll / std::max(offsets.back() - content, 1.0f) * (content - thumb);
            rrect(kPanelX + kPanelW - 5.0f, top + at, 1.6f, thumb, 0.8f, kWhite, 0.3f);
        }
        if (!v.hint.empty()) {
            const float size = 6.2f;
            const float hy = kPanelY + h - 7.5f;
            rrect(x, hy - 9.5f, w, 0.5f, 0.25f, kWhite, 0.07f); // hairline above the hint
            text(x + (w - measure(v.hint, size)) * 0.5f, hy, fit(v.hint, size, w), size, kDim, 0.85f);
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
