// Button prompts (frontend/button_prompts.hpp): painted over a made-up atlas, at the game's size and at a pack's.

#include "button_prompts.hpp"
#include "check.hpp"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <vector>

using namespace mhf::psp::prompts;

namespace {

constexpr std::uint32_t kClear = 0x00000000u, kDisc = 0xFF303030u, kRim = 0xFF808080u, kSymbol = 0xFFE0E0E0u;
constexpr std::uint32_t kPlate = 0xFFC0C0C0u, kEdge = 0xFF202020u, kInk = 0xFF000000u, kElsewhere = 0xFF123456u;

struct Picture {
    unsigned scale;
    std::vector<std::uint32_t> pixels;
    [[nodiscard]] unsigned side() const { return 64u * scale; }
    [[nodiscard]] std::uint32_t &at(unsigned x, unsigned y) { return pixels[y * side() + x]; }
    // A texel of the atlas at the PSP's size: every pixel of it.
    void set(unsigned x, unsigned y, std::uint32_t colour) {
        for (unsigned dy = 0; dy < scale; ++dy) {
            for (unsigned dx = 0; dx < scale; ++dx) at(x * scale + dx, y * scale + dy) = colour;
        }
    }
};

// A 64 x 64 atlas: four face pieces on a row (a disc each, with a different light symbol), an L and an R plate with
// dark lettering, START and SELECT with light lettering, and something else under them.
Atlas layout() {
    Atlas a;
    a.width = a.height = 64;
    a.cells = {{Icon::Circle, {0, 0, 16, 16}, {}, 0},   {Icon::Cross, {16, 0, 16, 16}, {}, 0},
               {Icon::Triangle, {32, 0, 16, 16}, {}, 0}, {Icon::Square, {48, 0, 16, 16}, {}, 0},
               {Icon::L, {0, 16, 20, 16}, {5, 20, 11, 8}, 28}, {Icon::R, {20, 16, 20, 16}, {25, 20, 11, 8}, 28},
               {Icon::Start, {0, 32, 20, 16}, {}, 0},     {Icon::Select, {20, 32, 20, 16}, {21, 37, 18, 5}, 36}};
    return a;
}

Picture picture(unsigned scale) {
    Picture p{scale, std::vector<std::uint32_t>(64u * scale * 64u * scale, kClear)};
    for (unsigned cell = 0; cell < 4; ++cell) {
        for (unsigned y = 0; y < 16; ++y) {
            for (unsigned x = 0; x < 16; ++x) {
                const int dx = static_cast<int>(2 * x) - 15, dy = static_cast<int>(2 * y) - 15, far = dx * dx + dy * dy;
                if (far > 15 * 15) continue;
                p.set(cell * 16 + x, y, far > 14 * 14 ? kRim : kDisc);
            }
        }
        // The symbols: a row, a column, a diagonal, a small box; they cross in the middle.
        for (unsigned i = 4; i < 12; ++i) {
            if (cell == 0) p.set(i, 8, kSymbol);
            if (cell == 1) p.set(16 + 8, i, kSymbol);
            if (cell == 2) p.set(32 + i, i, kSymbol);
            if (cell == 3) p.set(48 + i, i == 4 || i == 11 ? 8 : 4 + (i % 2) * 7, kSymbol);
        }
        p.set(cell * 16 + 8, 8, kSymbol); // all four cross here
    }
    for (unsigned plate = 0; plate < 2; ++plate) {
        for (unsigned y = 18; y < 30; ++y) {
            for (unsigned x = 1; x < 19; ++x) p.set(plate * 20 + x, y, x == 1 || x == 18 || y == 18 || y == 29 ? kEdge : kPlate);
        }
        for (unsigned y = 20; y < 28; ++y) p.set(plate * 20 + 7, y, kInk);             // a stem both letters have
        for (unsigned x = 7; x < 12; ++x) p.set(plate * 20 + x, plate == 0 ? 27 : 20, kInk); // L's foot, R's top
        for (unsigned y = 34; y < 46; ++y) {
            for (unsigned x = 0; x < 20; ++x) p.set(plate * 20 + x, y, x == 0 || x == 19 ? kRim : kDisc);
        }
        for (unsigned x = 2; x < 18; x += 2 + plate) p.set(plate * 20 + x, 39, kSymbol);    // the lettering, in its row
    }
    for (unsigned y = 48; y < 64; ++y) {
        for (unsigned x = 0; x < 64; ++x) p.set(x, y, kElsewhere);
    }
    return p;
}

bool in_box(const Box &b, unsigned x, unsigned y) { return x >= b.x && x < unsigned(b.x + b.w) && y >= b.y && y < unsigned(b.y + b.h); }

int channel(std::uint32_t colour, int shift) { return static_cast<int>((colour >> shift) & 0xFFu); }

void check_scale(unsigned scale) {
    const Atlas atlas = layout();
    const Picture before = picture(scale);
    // L and R bound to a bumper alone each: their plates are named. (The default binding leaves them: see main.)
    Labels named;
    named.left = shoulder_label("left_shoulder");
    named.right = shoulder_label("right_shoulder");
    Picture after = before;
    MHF_CHECK(repaint(atlas, Style::Xbox, after.pixels, scale, named));
    const unsigned side = before.side();

    // Nothing outside the pieces changed, and START (the same word on both pads) is as it was.
    bool outside_same = true, start_same = true;
    for (unsigned y = 0; y < side; ++y) {
        for (unsigned x = 0; x < side; ++x) {
            const unsigned tx = x / scale, ty = y / scale;
            bool in_piece = false;
            for (const Cell &c : atlas.cells) in_piece = in_piece || (c.icon != Icon::Start && in_box(c.box, tx, ty));
            const bool same = after.pixels[y * side + x] == before.pixels[y * side + x];
            if (!in_piece) outside_same = outside_same && same;
            if (in_box(atlas.cells[6].box, tx, ty)) start_same = start_same && same;
        }
    }
    MHF_CHECK(outside_same);
    MHF_CHECK(start_same);

    // The faces: the symbols are gone (also where all four crossed), the disc and what is clear around it stay, and
    // each letter is in its button's colour: B red, A green, Y yellow, X blue.
    unsigned left_light = 0, coloured[4] = {};
    bool clear_kept = true, rim_kept = true;
    for (unsigned cell = 0; cell < 4; ++cell) {
        for (unsigned y = 0; y < 16 * scale; ++y) {
            for (unsigned x = 0; x < 16 * scale; ++x) {
                const std::uint32_t was = before.pixels[y * side + cell * 16 * scale + x], now = after.pixels[y * side + cell * 16 * scale + x];
                if (now == kSymbol) ++left_light;
                if (was == kClear) clear_kept = clear_kept && now == kClear;
                if (was == kRim) rim_kept = rim_kept && now == kRim;
                const int r = channel(now, 0), g = channel(now, 8), b = channel(now, 16);
                const bool strong = std::max({r, g, b}) > 180;
                if (cell == 0 && strong && r > g + 80 && r > b + 80) ++coloured[0];
                if (cell == 1 && strong && g > r + 60 && g > b + 60) ++coloured[1];
                if (cell == 2 && strong && r > b + 100 && g > b + 100) ++coloured[2];
                if (cell == 3 && strong && b > r + 80 && b > g + 40) ++coloured[3];
            }
        }
    }
    MHF_CHECK(left_light == 0);
    MHF_CHECK(clear_kept);
    MHF_CHECK(rim_kept); // a letter stays inside the disc
    for (const unsigned count : coloured) MHF_CHECK(count >= 8 * scale * scale);

    // The plates: lettering in the plate's own ink, more of it for two letters (LB, RB) and four (BACK) than there
    // was, inside the lettering's box only; the plate's edge is as it was.
    const auto count_in = [&](const Picture &p, const Box &b, std::uint32_t colour) {
        unsigned n = 0;
        for (unsigned y = b.y * scale; y < unsigned(b.y + b.h) * scale; ++y) {
            for (unsigned x = b.x * scale; x < unsigned(b.x + b.w) * scale; ++x) n += p.pixels[y * side + x] == colour ? 1u : 0u;
        }
        return n;
    };
    for (const std::size_t i : {4u, 5u}) MHF_CHECK(count_in(after, atlas.cells[i].text, kInk) > count_in(before, atlas.cells[i].text, kInk));
    MHF_CHECK(count_in(after, atlas.cells[7].text, kSymbol) > count_in(before, atlas.cells[7].text, kSymbol));
    bool plate_kept = true; // a plate outside its lettering's box
    for (const std::size_t i : {4u, 5u, 7u}) {
        const Box &box = atlas.cells[i].box;
        for (unsigned y = box.y * scale; y < unsigned(box.y + box.h) * scale; ++y) {
            for (unsigned x = box.x * scale; x < unsigned(box.x + box.w) * scale; ++x) {
                if (!in_box(atlas.cells[i].text, x / scale, y / scale)) plate_kept = plate_kept && after.pixels[y * side + x] == before.pixels[y * side + x];
            }
        }
    }
    MHF_CHECK(plate_kept);
    // L and R got different letters.
    bool plates_differ = false;
    for (unsigned y = 16 * scale; y < 32 * scale; ++y) {
        for (unsigned x = 0; x < 20 * scale; ++x) plates_differ = plates_differ || after.pixels[y * side + x] != after.pixels[y * side + x + 20 * scale];
    }
    MHF_CHECK(plates_differ);

    // Painting is the same every time, and other labels (a pad whose buttons are bound otherwise) change the faces.
    Picture again = before;
    MHF_CHECK(repaint(atlas, Style::Xbox, again.pixels, scale, named) && again.pixels == after.pixels);
    Labels faces = named;
    faces.face[0] = 'A';
    faces.face[1] = 'B';
    Picture relabelled = before;
    MHF_CHECK(repaint(atlas, Style::Xbox, relabelled.pixels, scale, faces) && relabelled.pixels != after.pixels);
}

} // namespace

int main() {
    check_scale(1);
    check_scale(4);

    const Atlas atlas = layout();
    const Picture before = picture(1);

    // The L and R plates are the game's own unless the pad binds that button to one bumper alone: the default binding
    // (a bumper and a trigger together), a trigger alone or anything else has no one name. Each plate goes by itself.
    MHF_CHECK(std::string_view(shoulder_label("left_shoulder")) == "LB" && std::string_view(shoulder_label("right_shoulder")) == "RB");
    for (const char *binding : {"left_shoulder+left_trigger", "right_shoulder+right_trigger", "left_trigger", "right_trigger",
                                "left_trigger+left_shoulder", "south", ""}) {
        MHF_CHECK(shoulder_label(binding) == nullptr);
    }
    const auto plate_same = [&](const Picture &p, std::size_t cell) {
        const Box &box = atlas.cells[cell].box;
        bool same = true;
        for (unsigned y = box.y; y < unsigned(box.y + box.h); ++y) {
            for (unsigned x = box.x; x < unsigned(box.x + box.w); ++x) same = same && p.pixels[y * 64 + x] == before.pixels[y * 64 + x];
        }
        return same;
    };
    Picture unnamed = before;
    MHF_CHECK(repaint(atlas, Style::Xbox, unnamed.pixels, 1)); // the default labels
    MHF_CHECK(plate_same(unnamed, 4) && plate_same(unnamed, 5) && !plate_same(unnamed, 7) && unnamed.pixels != before.pixels);
    Labels left_only;
    left_only.left = shoulder_label("left_shoulder");
    left_only.right = shoulder_label("right_shoulder+right_trigger");
    Picture one = before;
    MHF_CHECK(repaint(atlas, Style::Xbox, one.pixels, 1, left_only) && !plate_same(one, 4) && plate_same(one, 5));

    // The game's own style, an image of another size, or a piece outside the image: nothing is painted.
    Picture same = before;
    MHF_CHECK(!repaint(atlas, Style::Original, same.pixels, 1) && same.pixels == before.pixels);
    MHF_CHECK(!repaint(atlas, Style::Xbox, same.pixels, 2) && same.pixels == before.pixels);
    MHF_CHECK(!repaint(atlas, Style::Xbox, same.pixels, 0) && same.pixels == before.pixels);
    Atlas outside = atlas;
    for (Cell &c : outside.cells) c.box.x = static_cast<std::uint16_t>(c.box.x + 200), c.text.x = static_cast<std::uint16_t>(c.text.x + 200);
    MHF_CHECK(repaint(outside, Style::Xbox, same.pixels, 1) && same.pixels == before.pixels);
    // Without all four faces there is no disc to take: the faces stay, the plates are still painted.
    Atlas three = atlas;
    three.cells.erase(three.cells.begin());
    Picture partial = before;
    MHF_CHECK(repaint(three, Style::Xbox, partial.pixels, 1));
    bool faces_same = true;
    for (unsigned i = 0; i < 16u * 64u; ++i) faces_same = faces_same && partial.pixels[i] == before.pixels[i];
    MHF_CHECK(faces_same && partial.pixels != before.pixels);

    // A texture's mark: the same bytes give the same mark, any other byte another.
    std::vector<std::uint8_t> bytes(4096);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i * 7u);
    const std::uint64_t mark = mark_of(bytes);
    MHF_CHECK(mark == mark_of(bytes));
    bytes[1234] ^= 1u;
    MHF_CHECK(mark != mark_of(bytes));
    MHF_CHECK(mark_of({}) != mark);

    return mhf::test::finish("psp_button_prompts");
}
