// sceLibFont with free fonts.
//
// The original system fonts are never used or included (see CONTRIBUTING.md). Instead, one free font (Liberation Sans, from
// third_party/fonts) is offered as the only installed font, rasterized with stb_truetype at the size of the original
// Latin system font (10.125 points at 128 dpi, i.e. an 18-pixel em). Characters it does not have (the kana and kanji
// of the Japanese release) are drawn with a second free font, Noto Sans JP, at the same em. Metrics follow the library's conventions: 26.6
// fixed point, glyph bitmaps positioned by the caller with bitmapLeft/bitmapTop, and glyph images drawn with their
// top-left corner at (xPos64, yPos64) >> 6.
//
// Structure layouts: pspsdk pspfont.h (SceFontInfo 0x108 bytes, SceFontCharacterInfo 0x3C, SceFontStyle 0xA8,
// SceFontImageBuffer 0x14).

#include "hle.hpp"

#include "mhf/psp/font_glyphs.hpp"
#include "stb_truetype.h"
#define STB_IMAGE_WRITE_STATIC // the font's glyph sheets (MHF_GLYPH_SHEETS); static, the frontend has its own copy
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <string>
#include <vector>

namespace mhf::psp::hle {

namespace {

constexpr const char *kFontFile = "LiberationSans-Regular.ttf";
constexpr const char *kFontName = "Liberation Sans";
constexpr const char *kJapaneseFontFile = "NotoSansJP-Regular.otf";
constexpr float kPoints = 10.125f;     // size of the original Latin font
constexpr float kResolution = 128.0f;  // dpi the library reports by default
constexpr std::uint32_t kAltCharacter = 0x5Fu;

constexpr std::uint32_t kLibraryHandle = 0x0A000000u; // opaque handles (outside guest RAM on purpose)
constexpr std::uint32_t kFontHandle = 0x0A000100u;
constexpr std::uint32_t kErrorInvalidParameter = 0x80460003u;
constexpr std::uint32_t kErrorNoFont = 0x80460005u;

struct Face {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    float scale{};
    int ascent{}, descent{}, line_gap{};

    // Overall glyph metrics in pixels (for sceFontGetFontInfo), computed over the Latin ranges.
    int max_width{}, max_height{}, max_advance{}, min_left{}, max_top{}, max_bottom{};
    int glyph_count{};
};

std::unique_ptr<Face> g_face;
std::unique_ptr<Face> g_japanese; // for the characters g_face does not have; may be missing
std::unique_ptr<Face> g_latin;    // with a player's font: Liberation Sans, for the characters that font does not have
std::string g_face_name = kFontName;
bool g_face_tried = false;
std::filesystem::path g_sheet_folder; // MHF_GLYPH_SHEETS: where the glyph sheets go (empty: none)
void write_sheet(const Face &fc, std::uint32_t block);

std::unique_ptr<Face> load_face(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return nullptr;
    auto result = std::make_unique<Face>();
    result->data.assign(std::istreambuf_iterator<char>(f), {});
    if (!stbtt_InitFont(&result->info, result->data.data(), stbtt_GetFontOffsetForIndex(result->data.data(), 0))) {
        std::fprintf(stderr, "[hle] sceLibFont: %s is not a usable TrueType font\n", path.string().c_str());
        return nullptr;
    }
    result->scale = stbtt_ScaleForMappingEmToPixels(&result->info, kPoints * kResolution / 72.0f);
    stbtt_GetFontVMetrics(&result->info, &result->ascent, &result->descent, &result->line_gap);
    return result;
}

Face *face(Runtime &rt) {
    if (g_face_tried) return g_face.get();
    g_face_tried = true;
    const auto path = rt.options().font_directory / kFontFile;
    std::unique_ptr<Face> result;
    if (const auto &custom = rt.options().custom_font; !custom.empty()) {
        result = load_face(custom);
        if (result) {
            const std::u8string name = custom.filename().u8string();
            g_face_name.assign(name.begin(), name.end());
            g_latin = load_face(path);
        } else {
            std::fprintf(stderr, "[hle] sceLibFont: the chosen font %s cannot be read; using %s\n", custom.string().c_str(),
                         kFontName);
        }
    }
    if (!result) result = load_face(path);
    if (!result) {
        std::fprintf(stderr, "[hle] sceLibFont: %s not found; text drawn with the font library will be missing\n",
                     path.string().c_str());
        return nullptr;
    }
    g_japanese = load_face(rt.options().font_directory / kJapaneseFontFile);
    Face &fc = *result;
    const auto measure = [&](std::uint32_t first, std::uint32_t last) {
        for (std::uint32_t c = first; c <= last; ++c) {
            const int glyph = stbtt_FindGlyphIndex(&fc.info, static_cast<int>(c));
            if (glyph == 0) continue;
            ++fc.glyph_count;
            int x0, y0, x1, y1, advance, bearing;
            stbtt_GetGlyphBitmapBox(&fc.info, glyph, fc.scale, fc.scale, &x0, &y0, &x1, &y1);
            stbtt_GetGlyphHMetrics(&fc.info, glyph, &advance, &bearing);
            fc.max_width = std::max(fc.max_width, x1 - x0);
            fc.max_height = std::max(fc.max_height, y1 - y0);
            fc.max_advance = std::max(fc.max_advance, static_cast<int>(std::lround(advance * fc.scale)));
            fc.min_left = std::min(fc.min_left, x0);
            fc.max_top = std::max(fc.max_top, -y0);
            fc.max_bottom = std::max(fc.max_bottom, y1);
        }
    };
    measure(0x20, 0x7E);
    measure(0xA0, 0x24F);
    std::fprintf(stderr, "[hle] sceLibFont: using %s (%d glyphs measured)%s\n", g_face_name.c_str(), fc.glyph_count,
                 g_japanese ? ", Japanese from Noto Sans JP" : "");
    g_face = std::move(result);
    if (const char *sheets = std::getenv("MHF_GLYPH_SHEETS"); sheets != nullptr && sheets[0] != '\0') {
        g_sheet_folder = sheets;
        // Latin text (accents included), the stars, the Japanese punctuation, kana and full-width forms
        for (const std::uint32_t block : {0x0000u, 0x0100u, 0x2000u, 0x2600u, 0x3000u, 0xFF00u}) write_sheet(*g_face, block);
        if (const char *all = std::getenv("MHF_GLYPH_SHEETS_ALL"); all != nullptr && all[0] == '1') {
            for (std::uint32_t block = 0x4E00u; block <= 0x9F00u; block += 0x100u) write_sheet(*g_face, block);
        }
    }
    return g_face.get();
}

std::int32_t fixed(float pixels) { return static_cast<std::int32_t>(std::lround(pixels * 64.0f)); }

void store_float(psprecomp::GuestMemory &mem, std::uint32_t address, float value) {
    mem.store32(address, std::bit_cast<std::uint32_t>(value));
}

void set_error(Runtime &rt, std::uint32_t pointer, std::uint32_t value) {
    if (pointer != 0) rt.memory().store32(pointer, value);
}

// SceFontStyle (0xA8 bytes) describing our single font.
void store_style(Runtime &rt, std::uint32_t address) {
    auto &mem = rt.memory();
    mem.zero(address, 0xA8);
    store_float(mem, address + 0x00, kPoints);
    store_float(mem, address + 0x04, kPoints);
    store_float(mem, address + 0x08, kResolution);
    store_float(mem, address + 0x0C, kResolution);
    store_float(mem, address + 0x10, 0.0f);
    mem.store16(address + 0x14, 1); // family: sans-serif
    mem.store16(address + 0x16, 1); // style: regular
    mem.store16(address + 0x18, 0);
    mem.store16(address + 0x1A, 2); // language: Latin ("English")
    mem.store16(address + 0x1C, 0);
    mem.store16(address + 0x1E, 0);
    const std::string name = kFontName, file = kFontFile;
    for (std::size_t i = 0; i < name.size() && i < 63; ++i) mem.store8(address + 0x20 + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(name[i]));
    for (std::size_t i = 0; i < file.size() && i < 63; ++i) mem.store8(address + 0x60 + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(file[i]));
}

// The stars of the original font (★ U+2605, ☆ U+2606), which the game uses for levels and ranks. Liberation Sans has no
// such glyph, so they are drawn here: a five-pointed star standing on the baseline, as tall as a capital letter.
constexpr std::uint32_t kBlackStar = 0x2605u, kWhiteStar = 0x2606u;
bool is_star(std::uint32_t code) { return code == kBlackStar || code == kWhiteStar; }

struct StarBox {
    int size{};    // width and height of the bitmap
    int x0{}, y0{}; // offset from the pen position (y0 negative: above the baseline)
    int advance{};
};

StarBox star_box(const Face &fc) {
    const int cap = std::max(6, static_cast<int>(std::lround(static_cast<float>(fc.ascent) * fc.scale * 0.72f)));
    return {cap, 1, -cap, cap + 2};
}

// Coverage of a star of `size` pixels, 4x4 samples per pixel; the white star keeps only a rim.
std::vector<unsigned char> star_bitmap(int size, bool filled) {
    constexpr double kPi = 3.14159265358979323846;
    const auto points = [&](double radius) {
        std::vector<std::pair<double, double>> p;
        const double c = size / 2.0, cy = size * 0.53;
        for (int i = 0; i < 10; ++i) {
            const double r = (i % 2 == 0 ? radius : radius * 0.40);
            const double a = -kPi / 2.0 + i * kPi / 5.0;
            p.emplace_back(c + r * std::cos(a), cy + r * std::sin(a));
        }
        return p;
    };
    const auto inside = [](const std::vector<std::pair<double, double>> &p, double x, double y) {
        bool in = false;
        for (std::size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
            if ((p[i].second > y) != (p[j].second > y) &&
                x < (p[j].first - p[i].first) * (y - p[i].second) / (p[j].second - p[i].second) + p[i].first) {
                in = !in;
            }
        }
        return in;
    };
    const auto outer = points(size * 0.5);
    const auto inner = points(std::max(0.0, size * 0.5 - std::max(1.2, size / 9.0)));
    std::vector<unsigned char> px(static_cast<std::size_t>(size * size));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const double fx = x + (sx + 0.5) / 4.0, fy = y + (sy + 0.5) / 4.0;
                    if (inside(outer, fx, fy) && (filled || !inside(inner, fx, fy))) ++hits;
                }
            }
            px[static_cast<std::size_t>(y * size + x)] = static_cast<unsigned char>(hits * 255 / 16);
        }
    }
    return px;
}

// Japanese-style characters the game's Latin text uses (full-width hyphen, katakana middle dot...), as the closest
// character Liberation Sans has: full-width ASCII as ASCII, the ideographic space as a space, the middle dot as U+00B7.
std::uint32_t latin_equivalent(std::uint32_t code) {
    if (code >= 0xFF01u && code <= 0xFF5Eu) return code - 0xFEE0u;
    if (code == 0x3000u) return 0x20u;
    if (code == 0x30FBu) return 0xB7u;
    return 0;
}

// A glyph and the font it comes from.
struct Glyph {
    const Face *face;
    int index;
};

Glyph glyph_for(const Face &fc, std::uint32_t code) {
    int glyph = stbtt_FindGlyphIndex(&fc.info, static_cast<int>(code));
    if (glyph == 0) {
        if (const std::uint32_t latin = latin_equivalent(code); latin != 0) glyph = stbtt_FindGlyphIndex(&fc.info, static_cast<int>(latin));
    }
    if (glyph == 0 && g_latin && &fc != g_latin.get()) {
        // A player's font without this character: Liberation Sans has the accents and symbols the game uses.
        int latin = stbtt_FindGlyphIndex(&g_latin->info, static_cast<int>(code));
        if (latin == 0) {
            if (const std::uint32_t plain = latin_equivalent(code); plain != 0) latin = stbtt_FindGlyphIndex(&g_latin->info, static_cast<int>(plain));
        }
        if (latin != 0) return {g_latin.get(), latin};
    }
    if (glyph == 0 && g_japanese) {
        if (const int japanese = stbtt_FindGlyphIndex(&g_japanese->info, static_cast<int>(code)); japanese != 0) {
            return {g_japanese.get(), japanese};
        }
    }
    if (glyph == 0 && code != 0x20u) {
        // Said once per character, so a report names what is missing.
        static std::set<std::uint32_t> reported;
        if (reported.insert(code).second) {
            std::fprintf(stderr, "[hle] sceLibFont: no font has U+%04X, drawn as '_'\n", static_cast<unsigned>(code));
        }
        glyph = stbtt_FindGlyphIndex(&fc.info, static_cast<int>(kAltCharacter));
    }
    return {&fc, glyph};
}

// ---------------------------------------------------------------- HD glyphs and glyph sheets (font_glyphs.hpp)

namespace fg = mhf::psp::font_glyphs;

// A glyph drawn at fg::kScale times the size, its pen `shift` (0..1) into its first pixel; kept, the game draws the
// same characters again and again (and each twice, a pixel apart, for its bold text).
std::shared_ptr<const fg::Coverage> hd_glyph(const Glyph &g, float shift_x, float shift_y, int &x0, int &y0) {
    static std::map<std::tuple<const Face *, int, int, int>, std::tuple<std::shared_ptr<const fg::Coverage>, int, int>> cache;
    const int sx = static_cast<int>(shift_x * 64.0f), sy = static_cast<int>(shift_y * 64.0f);
    const auto key = std::make_tuple(g.face, g.index, sx, sy);
    if (const auto it = cache.find(key); it != cache.end()) {
        x0 = std::get<1>(it->second);
        y0 = std::get<2>(it->second);
        return std::get<0>(it->second);
    }
    const float scale = g.face->scale * static_cast<float>(fg::kScale);
    int x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(&g.face->info, g.index, scale, scale, shift_x, shift_y, &x0, &y0, &x1, &y1);
    auto cov = std::make_shared<fg::Coverage>();
    cov->width = std::max(0, x1 - x0);
    cov->height = std::max(0, y1 - y0);
    cov->values.resize(static_cast<std::size_t>(cov->width) * static_cast<std::size_t>(cov->height));
    if (!cov->values.empty()) {
        stbtt_MakeGlyphBitmapSubpixel(&g.face->info, cov->values.data(), cov->width, cov->height, cov->width, scale, scale,
                                      shift_x, shift_y, g.index);
    }
    if (cache.size() > 8192) cache.clear(); // a long session of Japanese text: start over rather than grow
    cache[key] = {cov, x0, y0};
    return cov;
}

// Whether a font has the character (the glyph sheets leave out what would be drawn as '_').
bool has_glyph(const Face &fc, std::uint32_t code) {
    if (is_star(code)) return true;
    if (stbtt_FindGlyphIndex(&fc.info, static_cast<int>(code)) != 0) return true;
    if (const std::uint32_t latin = latin_equivalent(code); latin != 0 && stbtt_FindGlyphIndex(&fc.info, static_cast<int>(latin)) != 0) return true;
    if (g_latin && &fc != g_latin.get() && has_glyph(*g_latin, code)) return true;
    return g_japanese && stbtt_FindGlyphIndex(&g_japanese->info, static_cast<int>(code)) != 0;
}

std::set<std::uint32_t> g_sheets_written;

// The font's sheet of the block of 256 code points from `block`: white glyphs on transparent, each with its pen at
// fg::kSheetPen of its cell, at the PSP's size, as glyphs-XXXX.png.
void write_sheet(const Face &fc, std::uint32_t block) {
    if (g_sheet_folder.empty() || !g_sheets_written.insert(block).second) return;
    constexpr int size = fg::kSheetSize;
    std::vector<std::uint32_t> rgba(static_cast<std::size_t>(size) * size, 0x00FFFFFFu);
    int drawn = 0;
    for (std::uint32_t i = 0; i < 256; ++i) {
        const std::uint32_t code = block + i;
        if (code < 0x21u || !has_glyph(fc, code)) continue;
        const int pen_x = static_cast<int>(i % 16u) * fg::kSheetCell + fg::kSheetPenX;
        const int pen_y = static_cast<int>(i / 16u) * fg::kSheetCell + fg::kSheetPenY;
        int x0 = 0, y0 = 0, w = 0, h = 0;
        std::vector<unsigned char> px;
        if (is_star(code)) {
            const StarBox b = star_box(fc);
            x0 = b.x0;
            y0 = b.y0;
            w = h = b.size;
            px = star_bitmap(b.size, code == kBlackStar);
        } else {
            const Glyph g = glyph_for(fc, code);
            int x1, y1;
            stbtt_GetGlyphBitmapBox(&g.face->info, g.index, g.face->scale, g.face->scale, &x0, &y0, &x1, &y1);
            w = x1 - x0;
            h = y1 - y0;
            if (w <= 0 || h <= 0) continue;
            px.resize(static_cast<std::size_t>(w * h));
            stbtt_MakeGlyphBitmap(&g.face->info, px.data(), w, h, w, g.face->scale, g.face->scale, g.index);
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int ox = pen_x + x0 + x, oy = pen_y + y0 + y;
                if (ox < 0 || oy < 0 || ox >= size || oy >= size) continue;
                rgba[static_cast<std::size_t>(oy) * size + static_cast<std::size_t>(ox)] =
                    0x00FFFFFFu | (static_cast<std::uint32_t>(px[static_cast<std::size_t>(y * w + x)]) << 24u);
            }
        }
        ++drawn;
    }
    if (drawn == 0) return;
    char name[32];
    std::snprintf(name, sizeof name, "glyphs-%04X.png", static_cast<unsigned>(block));
    std::error_code ec;
    std::filesystem::create_directories(g_sheet_folder, ec);
    const std::string path = (g_sheet_folder / name).string();
    if (stbi_write_png(path.c_str(), size, size, 4, rgba.data(), size * 4) != 0) {
        std::fprintf(stderr, "[hle] sceLibFont: glyph sheet %s (%d characters)\n", path.c_str(), drawn);
    }
}

// ---------------------------------------------------------------- functions

void FontNewLib(Runtime &rt, Context &ctx) {
    set_error(rt, arg(ctx, 1), 0);
    face(rt);
    ret(ctx, kLibraryHandle);
}

void FontDoneLib(Runtime &, Context &ctx) { ret(ctx, 0); }

void FontGetNumFontList(Runtime &rt, Context &ctx) {
    set_error(rt, arg(ctx, 1), 0);
    ret(ctx, face(rt) != nullptr ? 1u : 0u);
}

void FontGetFontList(Runtime &rt, Context &ctx) {
    // (libHandle, SceFontStyle *list, int count)
    if (face(rt) != nullptr && arg(ctx, 1) != 0 && sarg(ctx, 2) > 0) store_style(rt, arg(ctx, 1));
    ret(ctx, 0);
}

void FontFindOptimumFont(Runtime &rt, Context &ctx) {
    if (face(rt) == nullptr) {
        set_error(rt, arg(ctx, 2), kErrorNoFont);
        ret(ctx, kErrorNoFont);
        return;
    }
    set_error(rt, arg(ctx, 2), 0);
    ret(ctx, 0); // the only font
}

void FontOpen(Runtime &rt, Context &ctx) {
    // (libHandle, index, mode, int *error)
    if (face(rt) == nullptr || arg(ctx, 1) != 0) {
        set_error(rt, arg(ctx, 3), kErrorNoFont);
        ret(ctx, 0);
        return;
    }
    set_error(rt, arg(ctx, 3), 0);
    ret(ctx, kFontHandle);
}

void FontClose(Runtime &, Context &ctx) { ret(ctx, 0); }

void FontGetFontInfo(Runtime &rt, Context &ctx) {
    const std::uint32_t out = arg(ctx, 1);
    Face *fc = face(rt);
    if (out == 0 || fc == nullptr) {
        ret(ctx, kErrorInvalidParameter);
        return;
    }
    auto &mem = rt.memory();
    mem.zero(out, 0x108);
    const float ascender = static_cast<float>(fc->ascent) * fc->scale;
    const float descender = static_cast<float>(fc->descent) * fc->scale;
    const float line = (static_cast<float>(fc->ascent - fc->descent + fc->line_gap)) * fc->scale;
    const float values[10] = {
        static_cast<float>(fc->max_width), static_cast<float>(fc->max_height), ascender, descender,
        static_cast<float>(fc->min_left), static_cast<float>(fc->max_top), static_cast<float>(fc->max_width) / 2.0f,
        static_cast<float>(fc->max_top), static_cast<float>(fc->max_advance), line};
    for (std::uint32_t i = 0; i < 10; ++i) {
        mem.store32(out + i * 4u, static_cast<std::uint32_t>(fixed(values[i])));
        store_float(mem, out + 40u + i * 4u, values[i]);
    }
    mem.store16(out + 80, static_cast<std::uint16_t>(fc->max_width));
    mem.store16(out + 82, static_cast<std::uint16_t>(fc->max_top + fc->max_bottom));
    mem.store32(out + 84, static_cast<std::uint32_t>(fc->glyph_count));
    mem.store32(out + 88, 0); // no shadow glyphs
    store_style(rt, out + 92);
    mem.store8(out + 260, 4); // 4 bits per pixel, like the original fonts
    ret(ctx, 0);
}

void FontGetCharInfo(Runtime &rt, Context &ctx) {
    const std::uint32_t code = arg(ctx, 1), out = arg(ctx, 2);
    Face *fc = face(rt);
    if (out == 0 || fc == nullptr) {
        ret(ctx, kErrorInvalidParameter);
        return;
    }
    auto &mem = rt.memory();
    mem.zero(out, 0x3C);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, advance = 0, bearing = 0;
    if (is_star(code)) {
        const StarBox b = star_box(*fc);
        x0 = b.x0;
        y0 = b.y0;
        x1 = b.x0 + b.size;
        y1 = b.y0 + b.size;
        advance = static_cast<int>(std::lround(b.advance / fc->scale));
    } else {
        const Glyph g = glyph_for(*fc, code);
        stbtt_GetGlyphBitmapBox(&g.face->info, g.index, g.face->scale, g.face->scale, &x0, &y0, &x1, &y1);
        stbtt_GetGlyphHMetrics(&g.face->info, g.index, &advance, &bearing);
        advance = static_cast<int>(std::lround(advance * g.face->scale / fc->scale)); // in units of the main font
    }
    const int width = x1 - x0, height = y1 - y0;
    const float line = static_cast<float>(fc->ascent - fc->descent + fc->line_gap) * fc->scale;
    mem.store32(out + 0, static_cast<std::uint32_t>(width));
    mem.store32(out + 4, static_cast<std::uint32_t>(height));
    mem.store32(out + 8, static_cast<std::uint32_t>(x0));  // bitmapLeft
    mem.store32(out + 12, static_cast<std::uint32_t>(-y0)); // bitmapTop (pixels above the baseline)
    const std::int32_t metrics[10] = {
        width * 64, height * 64,
        fixed(static_cast<float>(fc->ascent) * fc->scale), fixed(static_cast<float>(fc->descent) * fc->scale),
        x0 * 64, -y0 * 64,                       // horizontal bearing
        -(width * 64) / 2, 0,                    // vertical bearing
        fixed(static_cast<float>(advance) * fc->scale), fixed(line)};
    for (std::uint32_t i = 0; i < 10; ++i) mem.store32(out + 16u + i * 4u, static_cast<std::uint32_t>(metrics[i]));
    ret(ctx, 0);
}

// Writes one 8-bit coverage value into the caller's image, keeping whatever is already brighter there.
void put_pixel(psprecomp::GuestMemory &mem, std::uint32_t format, std::uint32_t buffer, std::uint32_t pitch, int x, int y,
               std::uint8_t value) {
    switch (format) {
    case 0: case 1: { // 4 bpp; format 0 keeps even pixels in the low nibble, format 1 in the high one
        const std::uint32_t at = buffer + static_cast<std::uint32_t>(y) * pitch + static_cast<std::uint32_t>(x) / 2u;
        const std::uint8_t old = mem.load8(at);
        const auto v = static_cast<std::uint8_t>(value >> 4u);
        const bool high = ((x & 1) != 0) != (format == 1);
        const std::uint8_t current = high ? static_cast<std::uint8_t>(old >> 4u) : static_cast<std::uint8_t>(old & 0xFu);
        if (v <= current) return;
        mem.store8(at, high ? static_cast<std::uint8_t>((old & 0x0Fu) | (v << 4u)) : static_cast<std::uint8_t>((old & 0xF0u) | v));
        return;
    }
    case 2: {
        const std::uint32_t at = buffer + static_cast<std::uint32_t>(y) * pitch + static_cast<std::uint32_t>(x);
        if (value > mem.load8(at)) mem.store8(at, value);
        return;
    }
    case 3: {
        const std::uint32_t at = buffer + static_cast<std::uint32_t>(y) * pitch + static_cast<std::uint32_t>(x) * 3u;
        if (value > mem.load8(at)) {
            mem.store8(at, value);
            mem.store8(at + 1, value);
            mem.store8(at + 2, value);
        }
        return;
    }
    case 4: {
        const std::uint32_t at = buffer + static_cast<std::uint32_t>(y) * pitch + static_cast<std::uint32_t>(x) * 4u;
        if (value > (mem.load32(at) & 0xFFu)) mem.store32(at, value * 0x01010101u);
        return;
    }
    default: return;
    }
}

void draw_glyph(Runtime &rt, Context &ctx, int clip_x, int clip_y, int clip_w, int clip_h) {
    const std::uint32_t code = arg(ctx, 1), image = arg(ctx, 2);
    Face *fc = face(rt);
    if (image == 0 || fc == nullptr) {
        ret(ctx, kErrorInvalidParameter);
        return;
    }
    auto &mem = rt.memory();
    const std::uint32_t format = mem.load32(image + 0);
    const auto x64 = static_cast<std::int32_t>(mem.load32(image + 4));
    const auto y64 = static_cast<std::int32_t>(mem.load32(image + 8));
    const int buffer_width = mem.load16(image + 12), buffer_height = mem.load16(image + 14);
    const std::uint32_t pitch = mem.load16(image + 16);
    const std::uint32_t buffer = mem.load32(image + 20);
    if (buffer == 0 || format > 4) {
        ret(ctx, kErrorInvalidParameter);
        return;
    }
    int width = 0, height = 0;
    std::vector<unsigned char> pixels;
    fg::Glyph placed; // for the HD text (font_glyphs.hpp)
    placed.code = code;
    const int left = x64 >> 6, top = y64 >> 6;
    int origin_x = left, origin_y = top; // where the bitmap's first pixel goes
    if (is_star(code)) {
        const StarBox b = star_box(*fc);
        width = height = b.size;
        pixels = star_bitmap(width, code == kBlackStar);
        placed.pen_x64 = (left - b.x0) * 64;
        placed.pen_y64 = (top - b.y0) * 64;
        placed.box_x = b.x0;
        placed.box_y = b.y0;
        placed.box_width = placed.box_height = b.size;
        auto hd = std::make_shared<fg::Coverage>();
        hd->width = hd->height = b.size * fg::kScale;
        const auto big = star_bitmap(hd->width, code == kBlackStar);
        hd->values.assign(big.begin(), big.end());
        placed.hd = std::move(hd);
        placed.hd_x = left * fg::kScale;
        placed.hd_y = top * fg::kScale;
    } else {
        const Glyph g = glyph_for(*fc, code);
        const float shift_x = static_cast<float>(x64 & 63) / 64.0f, shift_y = static_cast<float>(y64 & 63) / 64.0f;
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBoxSubpixel(&g.face->info, g.index, g.face->scale, g.face->scale, shift_x, shift_y, &x0, &y0, &x1,
                                        &y1);
        width = x1 - x0;
        height = y1 - y0;
        if (width <= 0 || height <= 0) {
            ret(ctx, 0);
            return;
        }
        pixels.resize(static_cast<std::size_t>(width * height));
        stbtt_MakeGlyphBitmapSubpixel(&g.face->info, pixels.data(), width, height, width, g.face->scale, g.face->scale, shift_x,
                                      shift_y, g.index);
        // The game asks for the glyph's top-left corner, worked out from the bearings sceFontGetCharInfo gave it (the
        // whole-pixel box): the pen is that far from the corner, `shift` into its pixel. The bitmap drawn at that shift
        // starts x0, y0 from the pen's pixel, which can be a pixel off the whole-pixel box: placing it by the pen keeps
        // every letter on the same baseline (placed by the corner, letters whose outline crosses a pixel edge at that
        // shift, such as b or f, sat a pixel high or low).
        int bx1, by1;
        stbtt_GetGlyphBitmapBox(&g.face->info, g.index, g.face->scale, g.face->scale, &placed.box_x, &placed.box_y, &bx1, &by1);
        placed.box_width = bx1 - placed.box_x;
        placed.box_height = by1 - placed.box_y;
        const int pen_px = left - placed.box_x, pen_py = top - placed.box_y;
        origin_x = pen_px + x0;
        origin_y = pen_py + y0;
        const float pen_x = static_cast<float>(pen_px) + shift_x, pen_y = static_cast<float>(pen_py) + shift_y;
        placed.pen_x64 = static_cast<std::int32_t>(std::lround(pen_x * 64.0f));
        placed.pen_y64 = static_cast<std::int32_t>(std::lround(pen_y * 64.0f));
        const float hd_pen_x = pen_x * static_cast<float>(fg::kScale), hd_pen_y = pen_y * static_cast<float>(fg::kScale);
        const float hd_origin_x = std::floor(hd_pen_x), hd_origin_y = std::floor(hd_pen_y);
        int hx0 = 0, hy0 = 0;
        placed.hd = hd_glyph(g, hd_pen_x - hd_origin_x, hd_pen_y - hd_origin_y, hx0, hy0);
        placed.hd_x = static_cast<int>(hd_origin_x) + hx0;
        placed.hd_y = static_cast<int>(hd_origin_y) + hy0;
    }
    // How many pixels fit in one line of the caller's buffer.
    const int per_line = format <= 1 ? static_cast<int>(pitch * 2u) : static_cast<int>(pitch / (format == 2 ? 1u : format == 3 ? 3u : 4u));
    const int max_x = std::min({buffer_width, per_line, clip_x + clip_w});
    const int max_y = std::min(buffer_height, clip_y + clip_h);
    placed.x = origin_x;
    placed.y = origin_y;
    placed.width = width;
    placed.height = height;
    placed.clip_x0 = std::max(0, clip_x);
    placed.clip_y0 = std::max(0, clip_y);
    placed.clip_x1 = max_x;
    placed.clip_y1 = max_y;
    if (format <= 1) fg::placed(buffer, buffer_width, buffer_height, std::move(placed)); // the game's text pages are 4-bit
    if (!g_sheet_folder.empty()) write_sheet(*fc, code & ~0xFFu);
    for (int yy = 0; yy < height; ++yy) {
        const int y = origin_y + yy;
        if (y < std::max(0, clip_y) || y >= max_y) continue;
        for (int xx = 0; xx < width; ++xx) {
            const int x = origin_x + xx;
            if (x < std::max(0, clip_x) || x >= max_x) continue;
            const std::uint8_t v = pixels[static_cast<std::size_t>(yy * width + xx)];
            if (v != 0) put_pixel(mem, format, buffer, pitch, x, y, v);
        }
    }
    ret(ctx, 0);
}

void FontGetCharGlyphImage(Runtime &rt, Context &ctx) { draw_glyph(rt, ctx, 0, 0, 1 << 16, 1 << 16); }

void FontGetCharGlyphImageClip(Runtime &rt, Context &ctx) {
    // (font, code, image, clipX, clipY, clipWidth, clipHeight)
    draw_glyph(rt, ctx, sarg(ctx, 3), sarg(ctx, 4), sarg(ctx, 5), sarg(ctx, 6));
}

void FontGetCharImageRect(Runtime &rt, Context &ctx) {
    const std::uint32_t out = arg(ctx, 2);
    Face *fc = face(rt);
    if (out == 0 || fc == nullptr) {
        ret(ctx, kErrorInvalidParameter);
        return;
    }
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (is_star(arg(ctx, 1))) {
        x1 = y1 = star_box(*fc).size;
    } else {
        const Glyph g = glyph_for(*fc, arg(ctx, 1));
        stbtt_GetGlyphBitmapBox(&g.face->info, g.index, g.face->scale, g.face->scale, &x0, &y0, &x1, &y1);
    }
    rt.memory().store16(out, static_cast<std::uint16_t>(x1 - x0));
    rt.memory().store16(out + 2, static_cast<std::uint16_t>(y1 - y0));
    ret(ctx, 0);
}

// Point/pixel conversions at the default resolution: (lib in $a0, value in $f12, int *error in $a1) -> $f0.
void point_pixel(Context &ctx, float factor) { ctx.fpr[0] = ctx.fpr[12] * factor; }
void FontPointToPixelH(Runtime &rt, Context &ctx) { set_error(rt, arg(ctx, 1), 0); point_pixel(ctx, kResolution / 72.0f); }
void FontPointToPixelV(Runtime &rt, Context &ctx) { set_error(rt, arg(ctx, 1), 0); point_pixel(ctx, kResolution / 72.0f); }
void FontPixelToPointH(Runtime &rt, Context &ctx) { set_error(rt, arg(ctx, 1), 0); point_pixel(ctx, 72.0f / kResolution); }
void FontPixelToPointV(Runtime &rt, Context &ctx) { set_error(rt, arg(ctx, 1), 0); point_pixel(ctx, 72.0f / kResolution); }

void FontZero(Runtime &, Context &ctx) { ret(ctx, 0); }

const HleFunction kFunctions[] = {
    {"sceLibFont", 0x67F17ED7u, "sceFontNewLib", &FontNewLib},
    {"sceLibFont", 0x574B6FBCu, "sceFontDoneLib", &FontDoneLib},
    {"sceLibFont", 0x27F6E642u, "sceFontGetNumFontList", &FontGetNumFontList},
    {"sceLibFont", 0xBC75D85Bu, "sceFontGetFontList", &FontGetFontList},
    {"sceLibFont", 0x099EF33Cu, "sceFontFindOptimumFont", &FontFindOptimumFont},
    {"sceLibFont", 0x681E61A7u, "sceFontFindFont", &FontFindOptimumFont},
    {"sceLibFont", 0xA834319Du, "sceFontOpen", &FontOpen},
    {"sceLibFont", 0x3AEA8CB6u, "sceFontClose", &FontClose},
    {"sceLibFont", 0x0DA7535Eu, "sceFontGetFontInfo", &FontGetFontInfo},
    {"sceLibFont", 0xDCC80C2Fu, "sceFontGetCharInfo", &FontGetCharInfo},
    {"sceLibFont", 0x980F4895u, "sceFontGetCharGlyphImage", &FontGetCharGlyphImage},
    {"sceLibFont", 0xCA1E6945u, "sceFontGetCharGlyphImage_Clip", &FontGetCharGlyphImageClip},
    {"sceLibFont", 0x5C3E4A9Eu, "sceFontGetCharImageRect", &FontGetCharImageRect},
    {"sceLibFont", 0x472694CDu, "sceFontPointToPixelH", &FontPointToPixelH},
    {"sceLibFont", 0x3C4B7E82u, "sceFontPointToPixelV", &FontPointToPixelV},
    {"sceLibFont", 0x74B21701u, "sceFontPixelToPointH", &FontPixelToPointH},
    {"sceLibFont", 0xF8F0752Eu, "sceFontPixelToPointV", &FontPixelToPointV},
    {"sceLibFont", 0x02D7F94Bu, "sceFontFlush", &FontZero},
    {"sceLibFont", 0x48293280u, "sceFontSetResolution", &FontZero},
    {"sceLibFont", 0xEE232411u, "sceFontSetAltCharacterCode", &FontZero},
};

} // namespace

std::span<const HleFunction> font_functions() { return kFunctions; }

} // namespace mhf::psp::hle

namespace mhf::psp::font_glyphs {

namespace {
std::mutex g_pages_mutex;
std::unordered_map<std::uint32_t, Page> g_pages;
std::uint32_t page_key(std::uint32_t address) { return address & 0x1FFFFFFFu; } // the same memory through any mirror
bool overlaps(const Glyph &a, const Glyph &b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}
} // namespace

void placed(std::uint32_t buffer, int width, int height, Glyph glyph) {
    std::lock_guard lock(g_pages_mutex);
    Page &page = g_pages[page_key(buffer)];
    if (page.width != width || page.height != height) page = Page{page.generation, width, height, {}}; // another page there now
    // The same character a pixel or two over is the game's bold text (drawn twice); anything else under the new glyph
    // is gone (its cell was cleared and reused).
    std::erase_if(page.glyphs, [&](const Glyph &old) {
        if (!overlaps(old, glyph)) return false;
        return old.code != glyph.code || std::abs(old.x - glyph.x) > 2 || std::abs(old.y - glyph.y) > 2;
    });
    if (page.glyphs.size() < 4096) page.glyphs.push_back(std::move(glyph));
    ++page.generation;
}

bool page(std::uint32_t address, Page &out) {
    std::lock_guard lock(g_pages_mutex);
    const auto it = g_pages.find(page_key(address));
    if (it == g_pages.end() || it->second.glyphs.empty()) return false;
    out = it->second;
    return true;
}

} // namespace mhf::psp::font_glyphs
