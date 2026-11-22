#include "button_prompts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace mhf::psp::prompts {
namespace {

struct Point {
    float x, y;
};
struct Stroke {
    Point a, b;
    bool round = false; // part of an arc
};

// A letter as strokes in a unit box (x across its width, y down its height), and how wide it is for its height.
struct Letter {
    std::vector<Stroke> strokes;
    float aspect = 0.7f;
};

void line(Letter &l, std::initializer_list<Point> points) {
    const Point *before = nullptr;
    for (const Point &p : points) {
        if (before != nullptr) l.strokes.push_back({*before, p});
        before = &p;
    }
}

// Part of an ellipse, from one angle to another (degrees, clockwise on screen from the right).
void arc(Letter &l, Point centre, float rx, float ry, float from, float to) {
    constexpr int kSteps = 12;
    constexpr float kRadians = 3.14159265f / 180.0f;
    Point before{};
    for (int i = 0; i <= kSteps; ++i) {
        const float angle = (from + (to - from) * static_cast<float>(i) / kSteps) * kRadians;
        const Point p{centre.x + rx * std::cos(angle), centre.y + ry * std::sin(angle)};
        if (i != 0) l.strokes.push_back({before, p, true});
        before = p;
    }
}

Letter letter(char c) {
    Letter l;
    switch (c) {
    case 'A':
        l.aspect = 0.8f;
        line(l, {{0, 1}, {0.5f, 0}, {1, 1}});
        line(l, {{0.2f, 0.66f}, {0.8f, 0.66f}});
        break;
    case 'B':
        l.aspect = 0.66f;
        line(l, {{0.55f, 0}, {0, 0}, {0, 1}, {0.55f, 1}});
        arc(l, {0.55f, 0.24f}, 0.4f, 0.24f, -90, 90);
        line(l, {{0, 0.48f}, {0.55f, 0.48f}});
        arc(l, {0.55f, 0.74f}, 0.45f, 0.26f, -90, 90);
        break;
    case 'C':
        l.aspect = 0.7f;
        arc(l, {0.56f, 0.5f}, 0.56f, 0.5f, 52, 308);
        break;
    case 'K':
        l.aspect = 0.68f;
        line(l, {{0, 0}, {0, 1}});
        line(l, {{1, 0}, {0, 0.6f}});
        line(l, {{0.38f, 0.4f}, {1, 1}});
        break;
    case 'L':
        l.aspect = 0.58f;
        line(l, {{0, 0}, {0, 1}, {1, 1}});
        break;
    case 'R':
        l.aspect = 0.66f;
        line(l, {{0, 1}, {0, 0}, {0.55f, 0}});
        arc(l, {0.55f, 0.25f}, 0.45f, 0.25f, -90, 90);
        line(l, {{0.55f, 0.5f}, {0, 0.5f}});
        line(l, {{0.5f, 0.5f}, {1, 1}});
        break;
    case 'X':
        l.aspect = 0.78f;
        line(l, {{0, 0}, {1, 1}});
        line(l, {{1, 0}, {0, 1}});
        break;
    case 'Y':
        l.aspect = 0.8f;
        line(l, {{0, 0}, {0.5f, 0.5f}, {1, 0}});
        line(l, {{0.5f, 0.5f}, {0.5f, 1}});
        break;
    default: break;
    }
    return l;
}

float distance(Point p, const Stroke &s) {
    const float vx = s.b.x - s.a.x, vy = s.b.y - s.a.y, wx = p.x - s.a.x, wy = p.y - s.a.y;
    const float along = std::clamp((wx * vx + wy * vy) / (vx * vx + vy * vy + 1e-12f), 0.0f, 1.0f);
    return std::hypot(wx - along * vx, wy - along * vy);
}

std::uint32_t channel(std::uint32_t colour, int shift) { return (colour >> shift) & 0xFFu; }

// `colour` over the texel, by how much of the texel the stroke covers (both with alpha of their own, not multiplied in).
std::uint32_t over(std::uint32_t under, std::uint32_t colour, float cover) {
    const float below = static_cast<float>(channel(under, 24)) / 255.0f * (1.0f - cover);
    const float alpha = cover + below;
    if (alpha <= 0.0f) return under;
    std::uint32_t out = static_cast<std::uint32_t>(std::lround(alpha * 255.0f)) << 24;
    for (int shift = 0; shift < 24; shift += 8) {
        const float mixed = (static_cast<float>(channel(colour, shift)) * cover + static_cast<float>(channel(under, shift)) * below) / alpha;
        out |= static_cast<std::uint32_t>(std::lround(std::clamp(mixed, 0.0f, 255.0f))) << shift;
    }
    return out;
}

struct Canvas {
    std::uint32_t *pixels;
    int width, height; // of the whole image
    [[nodiscard]] std::uint32_t &at(int x, int y) const { return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]; }
};

struct Rect {
    int x, y, w, h;
};

Rect scaled(const Box &b, int scale) { return {b.x * scale, b.y * scale, b.w * scale, b.h * scale}; }

// `text` in the middle of `box`, its letters `rows` texels tall with strokes `thick` texels thick, never outside the
// box. At the game's own size (snap) every stroke ends on the middle of a texel, so upright and level strokes are whole
// texels, as in the game's own small lettering.
void write(const Canvas &canvas, Rect box, std::string_view text, float rows, float thick, std::uint32_t colour, bool snap) {
    std::vector<Letter> letters;
    for (const char c : text) letters.push_back(letter(c));
    if (letters.empty()) return;
    const float half = thick * 0.5f, grow = std::max(0.0f, half - 0.5f); // a stroke reaches this far past its ends
    float gap = std::max(1.0f, rows * 0.2f);
    if (snap) {
        rows = std::round(rows);
        gap = std::round(gap);
    }
    std::vector<float> widths;
    float total = gap * static_cast<float>(letters.size() - 1) + 2.0f * grow * static_cast<float>(letters.size());
    for (const Letter &l : letters) {
        widths.push_back(snap ? std::max(2.0f, std::round(rows * l.aspect)) : rows * l.aspect);
        total += widths.back();
    }
    // Too wide for the box: narrower letters, the same height.
    if (const float room = static_cast<float>(box.w); total > room) {
        const float fixed = total - [&] { float sum = 0; for (const float w : widths) sum += w; return sum; }();
        const float squeeze = std::max(0.1f, (room - fixed)) / (total - fixed);
        total = fixed;
        for (float &w : widths) {
            w = snap ? std::max(2.0f, std::floor(w * squeeze)) : w * squeeze;
            total += w;
        }
    }
    float left = static_cast<float>(box.x) + (static_cast<float>(box.w) - total) * 0.5f + grow;
    float top = static_cast<float>(box.y) + (static_cast<float>(box.h) - rows) * 0.5f;
    if (snap) {
        left = std::round(left);
        top = std::round(top);
    }
    std::vector<Stroke> strokes;
    for (std::size_t i = 0; i < letters.size(); ++i) {
        const auto place = [&](Point p, bool to_texel) {
            Point out{left + 0.5f + p.x * (widths[i] - 1.0f), top + 0.5f + p.y * (rows - 1.0f)};
            if (to_texel) out = {std::floor(out.x) + 0.5f, std::floor(out.y) + 0.5f};
            return out;
        };
        for (const Stroke &s : letters[i].strokes) {
            const bool to_texel = snap && !s.round;
            strokes.push_back({place(s.a, to_texel), place(s.b, to_texel)});
        }
        left += widths[i] + gap + 2.0f * grow;
    }
    const int x1 = std::max(box.x, 0), y1 = std::max(box.y, 0);
    const int x2 = std::min(box.x + box.w, canvas.width), y2 = std::min(box.y + box.h, canvas.height);
    for (int y = y1; y < y2; ++y) {
        for (int x = x1; x < x2; ++x) {
            const Point p{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
            float nearest = 1e9f;
            for (const Stroke &s : strokes) nearest = std::min(nearest, distance(p, s));
            const float cover = std::clamp(0.5f - (nearest - half), 0.0f, 1.0f);
            if (cover > 0.0f) canvas.at(x, y) = over(canvas.at(x, y), colour, cover);
        }
    }
}

int luminance(std::uint32_t colour) {
    return static_cast<int>(channel(colour, 0) * 3u + channel(colour, 8) * 6u + channel(colour, 16));
}

int difference(std::uint32_t a, std::uint32_t b) {
    int sum = 0;
    for (int shift = 0; shift < 32; shift += 8) sum += std::abs(static_cast<int>(channel(a, shift)) - static_cast<int>(channel(b, shift)));
    return sum;
}

// The colours of the Xbox pad's face buttons (red in the low byte).
std::uint32_t face_colour(char label) {
    switch (label) {
    case 'A': return 0xFF50D67Cu; // green
    case 'B': return 0xFF4850F0u; // red
    case 'X': return 0xFFFF9A48u; // blue
    case 'Y': return 0xFF32CEFAu; // yellow
    default: return 0xFFFFFFFFu;
    }
}

const Cell *find(const Atlas &atlas, Icon icon) {
    for (const Cell &c : atlas.cells) {
        if (c.icon == icon) return &c;
    }
    return nullptr;
}

bool inside(const Rect &r, const Canvas &canvas) {
    return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 && r.x + r.w <= canvas.width && r.y + r.h <= canvas.height;
}

// The four face pieces become the disc they share with a letter on it.
void paint_faces(const Atlas &atlas, const Canvas &canvas, int scale, const Labels &labels) {
    constexpr Icon kFaces[4] = {Icon::Circle, Icon::Cross, Icon::Triangle, Icon::Square};
    Rect cells[4];
    for (int i = 0; i < 4; ++i) {
        const Cell *cell = find(atlas, kFaces[i]);
        if (cell == nullptr) return;
        cells[i] = scaled(cell->box, scale);
        if (!inside(cells[i], canvas) || cells[i].w != cells[0].w || cells[i].h != cells[0].h) return;
    }
    const int w = cells[0].w, h = cells[0].h;
    // The disc: a texel most of the four leave clear is clear; else the darkest of them is the disc (the symbols are
    // light on it).
    std::vector<std::uint32_t> disc(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    std::vector<bool> marked(disc.size()); // a symbol may be here: the four differ, or it is the middle of the piece
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint32_t clear = 0, darkest = 0;
            int clear_count = 0, dark = 1 << 30;
            const float dx = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) - 0.5f, dy = (static_cast<float>(y) + 0.5f) / static_cast<float>(h) - 0.5f;
            bool differ = dx * dx + dy * dy < 0.3f * 0.3f;
            for (const Rect &cell : cells) {
                const std::uint32_t texel = canvas.at(cell.x + x, cell.y + y);
                differ = differ || texel != canvas.at(cells[0].x + x, cells[0].y + y);
                if (channel(texel, 24) < 128u) {
                    if (clear_count++ == 0) clear = texel;
                } else if (luminance(texel) < dark) {
                    dark = luminance(texel);
                    darkest = texel;
                }
            }
            disc[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = clear_count >= 2 ? clear : darkest;
            marked[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = differ;
        }
    }
    // Where all four symbols cross (the lower corners of the square, the triangle, the ring and the cross) a few light
    // texels are left: a texel inside the disc where a symbol may be, much lighter than those around it, takes their
    // middle one. The disc's own light rim, the same in all four away from the middle, is never one of them.
    const auto texel = [&](int x, int y) -> std::uint32_t & { return disc[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; };
    for (int pass = 0; pass < 2 * scale; ++pass) {
        const std::vector<std::uint32_t> before = disc;
        const auto old = [&](int x, int y) { return before[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; };
        for (int y = 1; y + 1 < h; ++y) {
            for (int x = 1; x + 1 < w; ++x) {
                std::array<std::uint32_t, 8> around{};
                std::size_t count = 0;
                bool inside_disc = channel(old(x, y), 24) >= 128u && marked[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
                for (int dy = -1; dy <= 1 && inside_disc; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const std::uint32_t n = old(x + dx, y + dy);
                        if (channel(n, 24) < 128u) inside_disc = false;
                        else around[count++] = n;
                    }
                }
                if (!inside_disc || count != 8) continue;
                std::sort(around.begin(), around.end(), [](std::uint32_t a, std::uint32_t b) { return luminance(a) < luminance(b); });
                if (luminance(old(x, y)) > luminance(around[3]) + 640) texel(x, y) = around[3]; // a quarter of the range
            }
        }
    }
    for (int i = 0; i < 4; ++i) {
        for (int y = 0; y < h; ++y) {
            std::copy_n(disc.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w), w, &canvas.at(cells[i].x, cells[i].y + y));
        }
        const char label[2] = {labels.face[i], 0};
        write(canvas, cells[i], label, static_cast<float>(h) * 0.56f, static_cast<float>(h) * 0.085f, face_colour(labels.face[i]), false);
    }
}

// A plate's lettering goes (a row of the plate without any is drawn over it) and `text` is written in its colour.
void paint_plate(const Cell &cell, const Canvas &canvas, int scale, std::string_view text) {
    const Rect box = scaled(cell.text, scale);
    const int clean = cell.clean_row * scale;
    if (!inside(box, canvas) || clean >= canvas.height) return;
    std::uint32_t ink = 0;
    int furthest = -1;
    for (int y = box.y; y < box.y + box.h; ++y) {
        for (int x = box.x; x < box.x + box.w; ++x) {
            const std::uint32_t plate = canvas.at(x, clean + (y - box.y) % scale);
            if (const int d = difference(canvas.at(x, y), plate); d > furthest) {
                furthest = d;
                ink = canvas.at(x, y);
            }
            canvas.at(x, y) = plate;
        }
    }
    const float rows = static_cast<float>(box.h);
    // Lettering as small as START's is a texel thick, on whole texels at the game's own size, as the game's is.
    const bool small = rows <= 6.0f * static_cast<float>(scale);
    write(canvas, box, text, rows, small ? static_cast<float>(scale) : rows * 0.17f, ink | 0xFF000000u, small && scale == 1);
}

} // namespace

const char *shoulder_label(std::string_view binding) noexcept {
    return binding == "left_shoulder" ? "LB" : binding == "right_shoulder" ? "RB" : nullptr;
}

std::uint64_t mark_of(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t hash = 0xCBF29CE484222325ull; // FNV-1a
    for (const std::uint8_t b : bytes) hash = (hash ^ b) * 0x100000001B3ull;
    return hash;
}

bool repaint(const Atlas &atlas, Style style, std::span<std::uint32_t> pixels, unsigned scale, const Labels &labels) {
    if (style != Style::Xbox || scale == 0 || scale > 64 || atlas.width == 0 || atlas.height == 0) return false;
    const Canvas canvas{pixels.data(), static_cast<int>(atlas.width * scale), static_cast<int>(atlas.height * scale)};
    if (pixels.size() != static_cast<std::size_t>(canvas.width) * static_cast<std::size_t>(canvas.height)) return false;
    const int s = static_cast<int>(scale);
    paint_faces(atlas, canvas, s, labels);
    for (const Cell &cell : atlas.cells) {
        switch (cell.icon) {
        case Icon::L:
            if (labels.left != nullptr) paint_plate(cell, canvas, s, labels.left);
            break;
        case Icon::R:
            if (labels.right != nullptr) paint_plate(cell, canvas, s, labels.right);
            break;
        case Icon::Select: paint_plate(cell, canvas, s, "BACK"); break;
        default: break; // START is the same word on both pads
        }
    }
    return true;
}

} // namespace mhf::psp::prompts
