// The glyphs sceLibFont draws into the game's text pages, for the GL frontend's HD text.
//
// The game draws its text from small 4-bit textures it fills with sceFontGetCharGlyphImage: a page built once at boot
// (digits, Latin letters) and a cache it fills as new characters appear (accented letters, kana, kanji). The runtime
// draws each glyph at the PSP's size as the game asks, and keeps here where it put it, which character it was, and the
// same glyph drawn at kScale times the size from the same font. The frontend draws a page at kScale times its size from
// these, so every character, wherever the game put it, is drawn sharp.
//
// Glyph sheets: an artist's own drawing of the characters can take the font's place, a sheet per block of 256 code
// points (glyphs-XXXX.png, XXXX the block's first code point in hex). A sheet is 16 x 16 cells of kSheetCell pixels at
// the PSP's size, at any whole multiple of it; character c is in cell c & 0xFF (row-major), drawn with its pen at
// kSheetPen inside the cell. MHF_GLYPH_SHEETS=<folder> writes the font's own sheets there (the blocks of Latin text,
// kana and the full-width forms at start, every other block once a glyph of it is drawn), to draw over;
// MHF_GLYPH_SHEETS_ALL=1 adds every block of kanji (U+4E00 to U+9FFF) at start.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace mhf::psp::font_glyphs {

constexpr int kScale = 8;      // the HD pages' size, in times the PSP's
constexpr int kSheetCell = 32; // a sheet cell, in the PSP's pixels
constexpr int kSheetPenX = 6, kSheetPenY = 24; // the pen inside a cell
constexpr int kSheetSize = kSheetCell * 16;    // a sheet at the PSP's size

struct Coverage {
    int width = 0, height = 0;
    std::vector<std::uint8_t> values; // width * height, 0..255
};

struct Glyph {
    std::uint32_t code = 0;
    int x = 0, y = 0, width = 0, height = 0;          // the box drawn into the page, in its texels
    int clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0; // where the game let it draw, in texels
    std::int32_t pen_x64 = 0, pen_y64 = 0;            // the pen, in 1/64 texels (26.6)
    int box_x = 0, box_y = 0, box_width = 0, box_height = 0; // the glyph's box from the pen, unshifted, PSP size
    int hd_x = 0, hd_y = 0;                           // where `hd` goes in the HD page, in its pixels
    std::shared_ptr<const Coverage> hd;               // the glyph at kScale times, from the font
};

struct Page {
    std::uint64_t generation = 0;
    int width = 0, height = 0; // the page's size in texels, as the game gave it
    std::vector<Glyph> glyphs;
};

// The runtime: a glyph drawn into the page at `buffer` (a guest address). A glyph that takes the place of others
// (the game reuses a cell of its cache) drops them.
void placed(std::uint32_t buffer, int width, int height, Glyph glyph);

// The frontend: the glyphs of the page at `address` (the texture's guest address). False when none was drawn there.
bool page(std::uint32_t address, Page &out);

} // namespace mhf::psp::font_glyphs
