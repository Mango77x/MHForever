#include "dialog_overlay.hpp"

#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <tuple>

namespace mhf::psp {

namespace {

// Palette (the launcher's: launcher/ui/src/styles.css).
struct Color {
    float r, g, b;
};
constexpr Color kText{0.949f, 0.957f, 0.973f};
constexpr Color kDim{0.604f, 0.639f, 0.698f};
constexpr Color kAccent{0.961f, 0.651f, 0.141f}; // amber
constexpr Color kDanger{1.0f, 0.353f, 0.373f};
constexpr Color kInk{0.067f, 0.059f, 0.051f};    // text on amber
constexpr Color kPanelTop{0.090f, 0.102f, 0.129f};
constexpr Color kPanelBottom{0.055f, 0.063f, 0.082f};
constexpr Color kWhite{1.0f, 1.0f, 1.0f};
constexpr Color kBlack{0.0f, 0.0f, 0.0f};

// Decodes one UTF-8 sequence; malformed bytes come out as U+FFFD.
std::uint32_t next_codepoint(const std::string &s, std::size_t &i) {
    const auto b = static_cast<unsigned char>(s[i++]);
    if (b < 0x80) return b;
    int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    std::uint32_t cp = b & (0x3F >> extra);
    for (; extra > 0; --extra) {
        if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
    }
    return cp;
}

} // namespace

struct DialogOverlay::Impl {
    std::vector<unsigned char> font_data;
    stbtt_fontinfo font{};
    bool font_ok = false;

    struct Glyph {
        int x0, y0, w, h;
        std::vector<unsigned char> bitmap;
    };
    std::map<std::tuple<std::uint32_t, int, int>, Glyph> glyphs; // codepoint, pixel size * 4, subpixel quarter

    // Current target.
    std::vector<std::uint32_t> *px = nullptr;
    int W = 0, H = 0;
    float s = 1.0f; // logical (PSP) pixel → image pixels

    // ------------------------------------------------------------ pixels
    // Premultiplied "over" in 8-bit integers: dst = src * a + dst * (1 - a), alpha included.
    static void blend_px(std::uint32_t &d, std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
        if (a == 0) return;
        const std::uint32_t inv = 255u - a;
        const auto mix = [&](std::uint32_t src, unsigned shift) {
            const std::uint32_t v = (src * a + ((d >> shift) & 0xFFu) * inv + 127u) / 255u;
            return std::min(v, 255u) << shift;
        };
        d = mix(r, 0) | mix(g, 8) | mix(b, 16) | mix(255u, 24);
    }
    static std::uint32_t byte(float v) { return static_cast<std::uint32_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f)); }

    void blend(int x, int y, Color c, float a) {
        if (a <= 0.0f || x < 0 || y < 0 || x >= W || y >= H) return;
        blend_px((*px)[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)], byte(c.r),
                 byte(c.g), byte(c.b), byte(std::min(a, 1.0f)));
    }

    // Rounded rectangle in logical units. `border` > 0 strokes instead of filling. `bottom` blends a vertical gradient.
    // Only the corners need the distance function; straight edges and the interior take fast paths.
    void rrect(float x, float y, float w, float h, float radius, Color top, float alpha, float border = 0.0f,
               const Color *bottom = nullptr) {
        const float X = x * s, Y = y * s, Wd = w * s, Hd = h * s, R = radius * s, B = border * s;
        const int x0 = std::max(0, static_cast<int>(std::floor(X))), x1 = std::min(W, static_cast<int>(std::ceil(X + Wd)));
        const int y0 = std::max(0, static_cast<int>(std::floor(Y))), y1 = std::min(H, static_cast<int>(std::ceil(Y + Hd)));
        const float cx = X + Wd * 0.5f, cy = Y + Hd * 0.5f, hx = Wd * 0.5f - R, hy = Hd * 0.5f - R;
        const std::uint32_t full = byte(std::min(alpha, 1.0f));
        for (int py = y0; py < y1; ++py) {
            const float fy = static_cast<float>(py) + 0.5f;
            Color c = top;
            if (bottom != nullptr) {
                const float t = std::clamp((fy - Y) / std::max(Hd, 1.0f), 0.0f, 1.0f);
                c = {top.r + (bottom->r - top.r) * t, top.g + (bottom->g - top.g) * t, top.b + (bottom->b - top.b) * t};
            }
            const std::uint32_t r = byte(c.r), g = byte(c.g), b = byte(c.b);
            std::uint32_t *row = px->data() + static_cast<std::size_t>(py) * static_cast<std::size_t>(W);
            const float qy = std::fabs(fy - cy) - hy;
            // Rows clear of the top/bottom edges: the interior is fully covered (fill) or untouched (stroke).
            const bool middle = qy + R < -(B > 0.0f ? B : 0.0f) - 1.0f;
            const float edge = (B > 0.0f ? B : 0.0f) + 1.0f; // pixels from the left/right edge needing coverage
            for (int pxx = x0; pxx < x1; ++pxx) {
                const float fx = static_cast<float>(pxx) + 0.5f;
                if (middle && fx > X + edge && fx < X + Wd - edge) {
                    if (B > 0.0f) {
                        pxx = static_cast<int>(std::ceil(X + Wd - edge)) - 1; // jump to the right edge
                        continue;
                    }
                    blend_px(row[pxx], r, g, b, full);
                    continue;
                }
                const float qx = std::fabs(fx - cx) - hx;
                float d;
                if (qx > 0.0f && qy > 0.0f) d = std::sqrt(qx * qx + qy * qy) - R;
                else d = std::max(qx, qy) - R;
                float cov;
                if (B > 0.0f) cov = std::clamp(0.5f - (std::fabs(d + B * 0.5f) - B * 0.5f), 0.0f, 1.0f);
                else cov = std::clamp(0.5f - d, 0.0f, 1.0f);
                if (cov > 0.0f) blend_px(row[pxx], r, g, b, byte(alpha * cov));
            }
        }
    }

    void fill_all(Color c, float alpha) {
        const std::uint32_t a = byte(alpha), inv = 255u - a;
        const std::uint32_t value = ((byte(c.r) * a / 255u)) | ((byte(c.g) * a / 255u) << 8u) |
                                    ((byte(c.b) * a / 255u) << 16u) | (a << 24u);
        for (auto &d : *px) {
            if (d == 0) d = value;
            else blend_px(d, byte(c.r), byte(c.g), byte(c.b), a);
        }
        (void)inv;
    }

    // ------------------------------------------------------------ text
    const Glyph &glyph(std::uint32_t cp, float pixel_size, float subx) {
        const int size_key = static_cast<int>(pixel_size * 4.0f + 0.5f);
        const int sub_key = static_cast<int>(subx * 4.0f) & 3;
        const auto key = std::make_tuple(cp, size_key, sub_key);
        if (auto it = glyphs.find(key); it != glyphs.end()) return it->second;
        Glyph g{};
        const float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(size_key) / 4.0f);
        int gi = stbtt_FindGlyphIndex(&font, static_cast<int>(cp));
        if (gi == 0 && cp != ' ') gi = stbtt_FindGlyphIndex(&font, '?');
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBoxSubpixel(&font, gi, scale, scale, static_cast<float>(sub_key) / 4.0f, 0.0f, &x0, &y0,
                                        &x1, &y1);
        g.x0 = x0;
        g.y0 = y0;
        g.w = std::max(0, x1 - x0);
        g.h = std::max(0, y1 - y0);
        g.bitmap.resize(static_cast<std::size_t>(g.w) * static_cast<std::size_t>(g.h));
        if (g.w > 0 && g.h > 0)
            stbtt_MakeGlyphBitmapSubpixel(&font, g.bitmap.data(), g.w, g.h, g.w, scale, scale,
                                          static_cast<float>(sub_key) / 4.0f, 0.0f, gi);
        return glyphs.emplace(key, std::move(g)).first->second;
    }

    float advance(std::uint32_t cp, float pixel_size) const {
        const float scale = stbtt_ScaleForPixelHeight(&font, pixel_size);
        int gi = stbtt_FindGlyphIndex(&font, static_cast<int>(cp));
        if (gi == 0 && cp != ' ') gi = stbtt_FindGlyphIndex(&font, '?');
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&font, gi, &adv, &lsb);
        return static_cast<float>(adv) * scale;
    }

    // Width in logical units of `text` at `size` logical pixels.
    float measure(const std::string &text, float size, bool bold = false) const {
        float w = 0.0f;
        for (std::size_t i = 0; i < text.size();) w += advance(next_codepoint(text, i), size * s);
        return w / s + (bold ? 0.4f : 0.0f);
    }

    // Draws `text` with its baseline at logical (x, baseline); returns the logical width.
    float text(float x, float baseline, const std::string &str, float size, Color c, float alpha = 1.0f,
               bool bold = false) {
        if (!font_ok) return 0.0f;
        const float pixel_size = size * s;
        const int passes = bold ? 2 : 1;
        for (int pass = 0; pass < passes; ++pass) {
            float pen = x * s + static_cast<float>(pass) * 0.4f * s;
            const float base = baseline * s;
            for (std::size_t i = 0; i < str.size();) {
                const std::uint32_t cp = next_codepoint(str, i);
                const float fl = std::floor(pen);
                const Glyph &g = glyph(cp, pixel_size, pen - fl);
                const int ox = static_cast<int>(fl) + g.x0, oy = static_cast<int>(std::lround(base)) + g.y0;
                for (int gy = 0; gy < g.h; ++gy)
                    for (int gx = 0; gx < g.w; ++gx) {
                        const unsigned char v = g.bitmap[static_cast<std::size_t>(gy * g.w + gx)];
                        if (v != 0) blend(ox + gx, oy + gy, c, alpha * static_cast<float>(v) * (1.0f / 255.0f));
                    }
                pen += advance(cp, pixel_size);
            }
        }
        return measure(str, size, bold);
    }

    // Shortens `str` with an ellipsis to fit `max_width`.
    std::string fit(const std::string &str, float size, float max_width) const {
        if (measure(str, size) <= max_width) return str;
        std::string out = str;
        while (!out.empty() && measure(out + "\xE2\x80\xA6", size) > max_width) {
            out.pop_back();
            while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
        }
        return out + "\xE2\x80\xA6";
    }

    // Greedy word wrap.
    std::vector<std::string> wrap(const std::string &str, float size, float max_width) const {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start <= str.size()) {
            std::size_t end = str.find('\n', start);
            if (end == std::string::npos) end = str.size();
            const std::string para = str.substr(start, end - start);
            std::string line;
            std::size_t i = 0;
            while (i < para.size()) {
                std::size_t j = para.find(' ', i);
                if (j == std::string::npos) j = para.size();
                const std::string word = para.substr(i, j - i);
                const std::string candidate = line.empty() ? word : line + " " + word;
                if (!line.empty() && measure(candidate, size) > max_width) {
                    lines.push_back(line);
                    line = word;
                } else {
                    line = candidate;
                }
                i = j + 1;
            }
            lines.push_back(line);
            start = end + 1;
        }
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
        return lines;
    }

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

DialogOverlay::DialogOverlay(const std::filesystem::path &font_directory) : impl_(std::make_unique<Impl>()) {
    std::ifstream f(font_directory / "LiberationSans-Regular.ttf", std::ios::binary);
    impl_->font_data.assign(std::istreambuf_iterator<char>(f), {});
    impl_->font_ok = !impl_->font_data.empty() &&
                     stbtt_InitFont(&impl_->font, impl_->font_data.data(),
                                    stbtt_GetFontOffsetForIndex(impl_->font_data.data(), 0)) != 0;
    if (!impl_->font_ok) std::fprintf(stderr, "[frontend] dialogs: no font in %s\n", font_directory.string().c_str());
}

DialogOverlay::~DialogOverlay() = default;

bool DialogOverlay::render(const DialogView &view, int scale, std::vector<std::uint32_t> &pixels) {
    Impl &d = *impl_;
    d.W = width(scale);
    d.H = height(scale);
    d.s = static_cast<float>(scale);
    pixels.assign(static_cast<std::size_t>(d.W) * static_cast<std::size_t>(d.H), 0u);
    d.px = &pixels;
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
    d.px = nullptr;
    return d.font_ok;
}

} // namespace mhf::psp
