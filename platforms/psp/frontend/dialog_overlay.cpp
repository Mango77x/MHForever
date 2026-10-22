#include "dialog_overlay.hpp"

#include "overlay/painter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace mhf::psp {

namespace {

using overlay::Color;
using overlay::kAccent;
using overlay::kBlack;
using overlay::kDanger;
using overlay::kDim;
using overlay::kInk;
using overlay::kPanelBottom;
using overlay::kPanelTop;
using overlay::kText;
using overlay::kWhite;

} // namespace

struct DialogOverlay::Impl : overlay::Painter {
    explicit Impl(const std::filesystem::path &font_directory) : Painter(font_directory) {}

    // ------------------------------------------------------------ components
    void panel(float x, float y, float w, float h, bool error) {
        rrect(x, y + 3.0f, w, h, 12.0f, kBlack, 0.35f); // shadow
        rrect(x, y, w, h, 12.0f, kPanelTop, 0.96f, 0.0f, &kPanelBottom);
        rrect(x, y, w, h, 12.0f, kWhite, 0.09f, 0.6f);
        rrect(x + 18.0f, y, w - 36.0f, 1.6f, 0.8f, error ? kDanger : kAccent, 0.95f); // accent line
    }

    void button(float x, float y, float w, float h, const std::string &label, bool selected) {
        if (selected) {
            rrect(x - 1.5f, y - 1.5f, w + 3.0f, h + 3.0f, h * 0.5f + 1.5f, kAccent, 0.25f); // focus glow
            rrect(x, y, w, h, h * 0.5f, kAccent, 1.0f);
        } else {
            rrect(x, y, w, h, h * 0.5f, kWhite, 0.06f);
            rrect(x, y, w, h, h * 0.5f, kWhite, 0.14f, 0.6f);
        }
        const float size = 10.0f;
        const float tw = measure(label, size, true);
        text(x + (w - tw) * 0.5f, y + h * 0.5f + size * 0.36f, label, size, selected ? kInk : kText, 1.0f, true);
    }

    void hint(float cx, float y, const std::string &str) {
        const float size = 8.0f;
        text(cx - measure(str, size) * 0.5f, y, str, size, kDim, 0.9f);
    }

    // A save slot card: title, detail and date.
    void slot_card(float x, float y, float w, float h, const DialogView::Item &item, bool selected) {
        if (selected) {
            rrect(x, y, w, h, 8.0f, kAccent, 0.12f);
            rrect(x, y, w, h, 8.0f, kAccent, 0.85f, 0.8f);
        } else {
            rrect(x, y, w, h, 8.0f, kWhite, 0.035f);
            rrect(x, y, w, h, 8.0f, kWhite, 0.08f, 0.6f);
        }
        float tx = x + 10.0f;
        if (item.is_new) {
            const float bw = measure("NEW", 7.0f, true) + 8.0f;
            rrect(tx, y + 7.0f, bw, 10.0f, 5.0f, kAccent, 1.0f);
            text(tx + 4.0f, y + 14.5f, "NEW", 7.0f, kInk, 1.0f, true);
            tx += bw + 6.0f;
        }
        const float date_w = item.date.empty() ? 0.0f : measure(item.date, 8.0f) + 8.0f;
        text(tx, y + 16.0f, fit(item.title, 10.5f, x + w - 10.0f - date_w - tx), 10.5f, kText, 1.0f, true);
        if (!item.date.empty()) text(x + w - 10.0f - measure(item.date, 8.0f), y + 16.0f, item.date, 8.0f, kDim);
        if (!item.detail.empty()) {
            std::string first = item.detail.substr(0, item.detail.find('\n'));
            text(x + 10.0f, y + h - 9.0f, fit(first, 8.5f, w - 20.0f), 8.5f, kDim);
        }
    }

    // ------------------------------------------------------------ dialogs
    void draw_message(const DialogView &v) {
        const float w = 300.0f, pad = 18.0f;
        const auto lines = wrap(v.message, 10.5f, w - pad * 2.0f);
        float h = pad;
        if (!v.title.empty()) h += 20.0f;
        h += static_cast<float>(lines.size()) * 15.0f;
        if (!v.items.empty()) h += 10.0f + 44.0f;
        h += 16.0f + 24.0f + pad;
        const float x = (480.0f - w) * 0.5f, y = std::max(8.0f, (272.0f - h) * 0.5f - 6.0f);
        panel(x, y, w, h, v.error);
        float cy = y + pad;
        if (!v.title.empty()) {
            text(x + pad, cy + 11.0f, v.title, 13.0f, kText, 1.0f, true);
            cy += 20.0f;
        }
        for (const auto &line : lines) {
            text(x + pad, cy + 11.0f, line, 10.5f, kText, 0.86f);
            cy += 15.0f;
        }
        if (!v.items.empty()) {
            cy += 10.0f;
            slot_card(x + pad, cy, w - pad * 2.0f, 44.0f, v.items[0], false);
            cy += 44.0f;
        }
        cy += 16.0f;
        const float bw = 84.0f, gap = 8.0f;
        const float total = static_cast<float>(v.buttons.size()) * bw + static_cast<float>(v.buttons.size() - 1) * gap;
        float bx = x + w - pad - total;
        for (std::size_t i = 0; i < v.buttons.size(); ++i) {
            button(bx, cy, bw, 24.0f, v.buttons[i], static_cast<int>(i) == v.selected);
            bx += bw + gap;
        }
        if (!v.hint.empty()) hint(240.0f, y + h + 14.0f, v.hint);
    }

    void draw_keyboard(const DialogView &v) {
        const float key_w = 28.0f, key_h = 16.0f, gap = 3.5f;
        const float grid_w = 10.0f * key_w + 9.0f * gap;
        const float pad = 14.0f, w = grid_w + pad * 2.0f;
        const float rows = static_cast<float>(v.grid.size());
        const float h = pad + 19.0f + 26.0f + 9.0f + rows * key_h + (rows - 1.0f) * gap + pad;
        const float x = (480.0f - w) * 0.5f, y = std::max(3.0f, (272.0f - h - 14.0f) * 0.5f);
        panel(x, y, w, h, false);
        float cy = y + pad;
        text(x + pad, cy + 11.0f, fit(v.title, 12.5f, grid_w), 12.5f, kText, 1.0f, true);
        cy += 19.0f;
        // Text field.
        rrect(x + pad, cy, grid_w, 24.0f, 7.0f, kBlack, 0.35f);
        rrect(x + pad, cy, grid_w, 24.0f, 7.0f, kAccent, 0.7f, 0.8f);
        const std::string counter = std::to_string(v.text.size()) + " / " + std::to_string(v.max_length);
        const float cw = measure(counter, 8.0f);
        text(x + pad + grid_w - 10.0f - cw, cy + 15.5f, counter, 8.0f, kDim);
        const float tw = text(x + pad + 10.0f, cy + 16.5f, v.text, 12.5f, kText);
        rrect(x + pad + 11.0f + tw, cy + 5.5f, 1.4f, 13.0f, 0.7f, kAccent, 1.0f); // caret
        cy += 26.0f + 9.0f;
        // Key grid.
        for (std::size_t r = 0; r < v.grid.size(); ++r) {
            const std::string &row = v.grid[r];
            const float n = static_cast<float>(row.size());
            const float kw = (grid_w - (n - 1.0f) * gap) / n;
            for (std::size_t c = 0; c < row.size(); ++c) {
                const float kx = x + pad + static_cast<float>(c) * (kw + gap);
                const bool sel = static_cast<int>(r) == v.grid_row && static_cast<int>(c) == v.grid_col;
                std::string label;
                switch (row[c]) {
                case '\x01': label = "Space"; break;
                case '\x02': label = "Delete"; break;
                case '\x03': label = "Done"; break;
                default: label = std::string(1, row[c]);
                }
                const bool special = row[c] >= '\x01' && row[c] <= '\x03';
                if (sel) {
                    rrect(kx - 1.2f, cy - 1.2f, kw + 2.4f, key_h + 2.4f, 6.2f, kAccent, 0.3f);
                    rrect(kx, cy, kw, key_h, 5.0f, kAccent, 1.0f);
                } else {
                    rrect(kx, cy, kw, key_h, 5.0f, kWhite, special && row[c] == '\x03' ? 0.10f : 0.055f);
                    rrect(kx, cy, kw, key_h, 5.0f, kWhite, 0.07f, 0.5f);
                }
                const float size = special ? 8.0f : 9.5f;
                const float lw = measure(label, size, sel);
                text(kx + (kw - lw) * 0.5f, cy + key_h * 0.5f + size * 0.36f, label, size, sel ? kInk : kText, 1.0f, sel);
            }
            cy += key_h + gap;
        }
        if (!v.hint.empty()) hint(240.0f, std::min(269.0f, y + h + 11.0f), v.hint);
    }

    void draw_list(const DialogView &v) {
        const float pad = 18.0f, w = 340.0f, row_h = 40.0f, gap = 6.0f;
        const int visible = std::min<int>(4, static_cast<int>(v.items.size()));
        const float h = pad + 22.0f + static_cast<float>(visible) * (row_h + gap) - gap + pad;
        const float x = (480.0f - w) * 0.5f, y = std::max(6.0f, (272.0f - h) * 0.5f - 6.0f);
        panel(x, y, w, h, false);
        text(x + pad, y + pad + 11.0f, v.title, 13.0f, kText, 1.0f, true);
        const int count = static_cast<int>(v.items.size());
        int first = std::clamp(v.selected - visible / 2, 0, std::max(0, count - visible));
        if (count > visible) {
            const std::string pos = std::to_string(v.selected + 1) + " / " + std::to_string(count);
            text(x + w - pad - measure(pos, 8.0f), y + pad + 10.0f, pos, 8.0f, kDim);
        }
        float cy = y + pad + 22.0f;
        for (int i = first; i < first + visible; ++i) {
            slot_card(x + pad, cy, w - pad * 2.0f, row_h, v.items[static_cast<std::size_t>(i)], i == v.selected);
            cy += row_h + gap;
        }
        if (!v.hint.empty()) hint(240.0f, std::min(268.0f, y + h + 14.0f), v.hint);
    }

    void draw_progress(const DialogView &v) {
        const float w = 220.0f, h = v.message.empty() ? 62.0f : 76.0f;
        const float x = (480.0f - w) * 0.5f, y = (272.0f - h) * 0.5f;
        panel(x, y, w, h, false);
        const float tw = measure(v.title, 12.0f, true);
        text(240.0f - tw * 0.5f, y + 26.0f, v.title, 12.0f, kText, 1.0f, true);
        if (!v.message.empty()) {
            const float mw = measure(v.message, 9.0f);
            text(240.0f - mw * 0.5f, y + 42.0f, v.message, 9.0f, kDim);
        }
        // Progress dots.
        for (int i = 0; i < 3; ++i)
            rrect(240.0f - 14.0f + static_cast<float>(i) * 11.0f, y + h - 18.0f, 6.0f, 6.0f, 3.0f, kAccent,
                  0.45f + 0.25f * static_cast<float>(i));
    }

    void draw_toast(const DialogView &v) {
        const float w = 176.0f, h = v.message.empty() ? 28.0f : 40.0f;
        const float x = 480.0f - 12.0f - w, y = 12.0f;
        rrect(x, y + 2.0f, w, h, 10.0f, kBlack, 0.35f);
        rrect(x, y, w, h, 10.0f, kPanelTop, 0.94f, 0.0f, &kPanelBottom);
        rrect(x, y, w, h, 10.0f, kWhite, 0.09f, 0.6f);
        rrect(x + 10.0f, y + h * 0.5f - 3.5f, 7.0f, 7.0f, 3.5f, kAccent, 0.3f);
        rrect(x + 11.5f, y + h * 0.5f - 2.0f, 4.0f, 4.0f, 2.0f, kAccent, 1.0f);
        text(x + 24.0f, y + (v.message.empty() ? 17.5f : 17.0f), fit(v.title, 10.0f, w - 34.0f), 10.0f, kText, 1.0f, true);
        if (!v.message.empty()) text(x + 24.0f, y + 30.0f, fit(v.message, 8.5f, w - 34.0f), 8.5f, kDim);
    }
};

DialogOverlay::DialogOverlay(const std::filesystem::path &font_directory)
    : impl_(std::make_unique<Impl>(font_directory)) {}

DialogOverlay::~DialogOverlay() = default;

bool DialogOverlay::render(const DialogView &view, int scale, std::vector<std::uint32_t> &pixels) {
    Impl &d = *impl_;
    d.begin(pixels, width(scale), height(scale), static_cast<float>(scale));
    using Kind = DialogView::Kind;
    if (view.kind != Kind::None && view.kind != Kind::Toast) d.fill_all(kBlack, 0.55f);
    switch (view.kind) {
    case Kind::Message: d.draw_message(view); break;
    case Kind::Keyboard: d.draw_keyboard(view); break;
    case Kind::List: d.draw_list(view); break;
    case Kind::Progress: d.draw_progress(view); break;
    case Kind::Toast: d.draw_toast(view); break;
    case Kind::None: break;
    }
    d.end();
    return d.font_ok();
}

} // namespace mhf::psp
