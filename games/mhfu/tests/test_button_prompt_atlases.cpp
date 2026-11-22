// The game's textures that hold its button prompts (host/button_prompt_atlases.hpp), from made-up TMH containers.

#include "button_prompt_atlases.hpp"
#include "check.hpp"

#include <algorithm>
#include <vector>

using namespace mhf::mhfu;
using mhf::psp::prompts::Icon;

namespace {

constexpr unsigned kImageSize = 16 + 16 + 65536 + 16 + 1024;

void put(std::vector<std::uint8_t> &b, std::size_t o, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b[o + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// A container of `count` swizzled 256 x 256 images of 8-bit texels; every texel is palette entry 0 (clear).
std::vector<std::uint8_t> container(unsigned count) {
    std::vector<std::uint8_t> b(16 + count * kImageSize);
    std::copy_n(".TMH0.14", 8, b.begin());
    put(b, 8, count);
    for (unsigned i = 0; i < count; ++i) {
        const auto o = 16 + i * kImageSize;
        put(b, o, kImageSize);
        put(b, o + 8, 1);
        put(b, o + 12, 1);
        put(b, o + 16, 65552);
        put(b, o + 20, 1);
        put(b, o + 24, 5);
        put(b, o + 28, 0x01000100);
        const auto p = o + 32 + 65536;
        put(b, p, 1040);
        put(b, p + 4, 2);
        put(b, p + 8, 3);
        put(b, p + 12, 256);
        for (unsigned k = 1; k < 256; ++k) put(b, p + 16 + 4 * k, 0xFF000000u | (k * 0x010101u));
    }
    return b;
}

void texel(std::vector<std::uint8_t> &b, unsigned image, unsigned x, unsigned y, std::uint8_t entry) {
    b[16 + image * kImageSize + 32 + (y / 8) * 2048 + (x / 16) * 128 + (y % 8) * 16 + x % 16] = entry;
}

// The pieces as the game lays them out from (x, y): four discs with a different symbol each, two plates with
// different lettering and, with `start`, two more under the discs.
void draw_pieces(std::vector<std::uint8_t> &b, unsigned image, unsigned x, unsigned y, bool start, bool plates = true) {
    for (unsigned cell = 0; cell < 4; ++cell) {
        for (unsigned dy = 1; dy < 15; ++dy) {
            for (unsigned dx = 1; dx < 15; ++dx) texel(b, image, x + cell * 16 + dx, y + dy, 40);
        }
        for (unsigned i = 4; i < 12; ++i) texel(b, image, x + cell * 16 + (cell % 2 ? i : 8), y + (cell / 2 ? i : 4 + cell), 200);
    }
    for (unsigned plate = 0; plate < 2 && plates; ++plate) {
        for (unsigned dy = 2; dy < 14; ++dy) {
            for (unsigned dx = 1; dx < 19; ++dx) texel(b, image, x + 64 + plate * 20 + dx, y + dy, 180);
        }
        for (unsigned i = 4; i < 12; ++i) texel(b, image, x + 64 + plate * 20 + 6 + plate * 3, y + i, 10);
    }
    for (unsigned plate = 0; plate < 2 && start; ++plate) {
        for (unsigned dy = 18; dy < 31; ++dy) {
            for (unsigned dx = 0; dx < 20; ++dx) texel(b, image, x + plate * 20 + dx, y + dy, 60);
        }
        for (unsigned dx = 2; dx < 18; dx += 2 + plate) texel(b, image, x + plate * 20 + dx, y + 23, 240);
    }
}

std::uint64_t mark_at(const std::vector<std::uint8_t> &b, unsigned image, unsigned offset, unsigned size) {
    const auto begin = b.begin() + 16 + image * kImageSize + 32 + offset;
    return mhf::psp::prompts::mark_of({&*begin, size});
}

bool has(const mhf::psp::prompts::Atlas &a, Icon icon) {
    return std::any_of(a.cells.begin(), a.cells.end(), [icon](const auto &c) { return c.icon == icon; });
}

} // namespace

int main() {
    auto hud = container(2), menu = container(5);
    draw_pieces(hud, 1, 0, 0, true);
    draw_pieces(menu, 3, 104, 144, false);

    // Both atlases: the HUD's with START and SELECT, the menus' without; each told by the rows its pieces are in.
    const auto both = prompt_atlases(hud, menu);
    MHF_CHECK(both.size() == 2);
    if (both.size() == 2) {
        const auto &h = both[0], &m = both[1];
        MHF_CHECK(h.width == 256 && h.height == 256 && h.cells.size() == 8);
        MHF_CHECK(h.mark_offset == 0 && h.mark_size == 8192 && h.mark == mark_at(hud, 1, 0, 8192));
        MHF_CHECK(has(h, Icon::Circle) && has(h, Icon::Square) && has(h, Icon::L) && has(h, Icon::R) && has(h, Icon::Start) && has(h, Icon::Select));
        MHF_CHECK(h.cells[0].box.x == 0 && h.cells[3].box.x == 48 && h.cells[3].box.w == 16 && h.cells[3].box.h == 16);
        MHF_CHECK(m.cells.size() == 6 && !has(m, Icon::Start) && !has(m, Icon::Select));
        MHF_CHECK(m.mark_offset == 18u * 2048u && m.mark_size == 4096 && m.mark == mark_at(menu, 3, 18 * 2048, 4096));
        MHF_CHECK(m.cells[0].box.x == 104 && m.cells[0].box.y == 144 && m.cells[4].box.x == 168 && m.cells[4].text.y == 148);
        MHF_CHECK(m.cells[4].clean_row == 156 && h.cells[7].clean_row == 20);
        MHF_CHECK(h.mark != m.mark);
        // Every lettering box is inside its plate.
        for (const auto &a : both) {
            for (const auto &c : a.cells) {
                if (c.text.w == 0) continue;
                MHF_CHECK(c.text.x >= c.box.x && c.text.y >= c.box.y && c.text.x + c.text.w <= c.box.x + c.box.w && c.text.y + c.text.h <= c.box.y + c.box.h);
                MHF_CHECK(c.clean_row >= c.box.y && c.clean_row < c.box.y + c.box.h);
            }
        }
    }

    // Either file alone, and the same pieces in another language's file (other texels elsewhere): the same mark.
    MHF_CHECK(prompt_atlases(hud, {}).size() == 1 && prompt_atlases({}, menu).size() == 1 && prompt_atlases({}, {}).empty());
    auto other_language = hud;
    texel(other_language, 1, 100, 200, 77);
    const auto other = prompt_atlases(other_language, {});
    MHF_CHECK(other.size() == 1 && both.size() == 2 && other[0].mark == both[0].mark);

    // Not the atlas: an empty image, the pieces somewhere else, four faces that are the same, or another kind of file.
    MHF_CHECK(prompt_atlases(container(2), container(5)).empty());
    auto moved = container(2);
    draw_pieces(moved, 1, 32, 64, true);
    MHF_CHECK(prompt_atlases(moved, {}).empty());
    auto same_faces = container(2);
    for (unsigned cell = 0; cell < 4; ++cell) {
        for (unsigned y = 1; y < 15; ++y) {
            for (unsigned x = 1; x < 15; ++x) texel(same_faces, 1, cell * 16 + x, y, 40);
        }
        texel(same_faces, 1, cell * 16 + 8, 4, 200);
    }
    MHF_CHECK(prompt_atlases(same_faces, {}).empty());
    MHF_CHECK(prompt_atlases(container(1), container(3)).empty()); // too few images
    auto truncated = hud;
    truncated.resize(truncated.size() - 100);
    MHF_CHECK(prompt_atlases(truncated, {}).empty());
    auto not_tmh = hud;
    not_tmh[1] = 'X';
    MHF_CHECK(prompt_atlases(not_tmh, {}).empty());
    auto other_format = hud;
    put(other_format, 16 + kImageSize + 24, 4);
    MHF_CHECK(prompt_atlases(other_format, {}).empty());

    // Faces without plates (a file whose plates are not there): the faces only.
    auto no_plates = container(2);
    draw_pieces(no_plates, 1, 0, 0, false, false);
    const auto faces_only = prompt_atlases(no_plates, {});
    MHF_CHECK(faces_only.size() == 1 && faces_only[0].cells.size() == 4);

    // No image, no executable: nothing, and no file ids: nothing read at all.
    MHF_CHECK(load_prompt_atlases("no-such.iso", "no-such.elf", 30, 25).empty());
    MHF_CHECK(load_prompt_atlases("no-such.iso", "no-such.elf", {}, {}).empty());

    return mhf::test::finish("mhfu_button_prompt_atlases");
}
