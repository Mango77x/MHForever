#pragma once

// CPU drawing for the overlays shown over a game (system dialogs, the in-game menu): rounded rectangles, anti-aliased
// text with a free font, in the launcher's style (dark surfaces, amber accent). Console-independent: coordinates are
// "logical" units of a canvas (480x272 for the PSP) scaled to the image size.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

struct stbtt_fontinfo;

namespace mhf::overlay {

// Palette (the launcher's: launcher/ui/src/styles.css).
struct Color {
    float r, g, b;
};
inline constexpr Color kText{0.949f, 0.957f, 0.973f};
inline constexpr Color kDim{0.604f, 0.639f, 0.698f};
inline constexpr Color kAccent{0.961f, 0.651f, 0.141f}; // amber
inline constexpr Color kDanger{1.0f, 0.353f, 0.373f};
inline constexpr Color kInk{0.067f, 0.059f, 0.051f};    // text on amber
inline constexpr Color kPanelTop{0.090f, 0.102f, 0.129f};
inline constexpr Color kPanelBottom{0.055f, 0.063f, 0.082f};
inline constexpr Color kWhite{1.0f, 1.0f, 1.0f};
inline constexpr Color kBlack{0.0f, 0.0f, 0.0f};

class Painter {
public:
    // `font_directory` holds LiberationSans-Regular.ttf, and NotoSansJP-Regular.otf for the characters it lacks
    // (Japanese save titles).
    explicit Painter(const std::filesystem::path &font_directory);
    ~Painter();
    Painter(const Painter &) = delete;
    Painter &operator=(const Painter &) = delete;

    [[nodiscard]] bool font_ok() const { return font_ok_; }

    // Starts drawing into `pixels` (cleared): premultiplied RGBA, R in the low byte, `width` x `height` pixels, where
    // one logical unit is `scale` pixels.
    void begin(std::vector<std::uint32_t> &pixels, int width, int height, float scale);
    void end() { px_ = nullptr; }

    // Rounded rectangle in logical units. `border` > 0 strokes instead of filling. `bottom` blends a vertical gradient.
    void rrect(float x, float y, float w, float h, float radius, Color top, float alpha, float border = 0.0f,
               const Color *bottom = nullptr);
    void fill_all(Color c, float alpha);

    // Width in logical units of `text` at `size` logical pixels; `tracking` is extra space after every glyph.
    [[nodiscard]] float measure(const std::string &text, float size, bool bold = false, float tracking = 0.0f) const;
    // Draws `text` with its baseline at logical (x, baseline); returns the logical width.
    float text(float x, float baseline, const std::string &str, float size, Color c, float alpha = 1.0f, bool bold = false,
               float tracking = 0.0f);
    // A square image (premultiplied RGBA, `source_size` pixels a side) drawn `size` logical units wide at (x, y). Shrinking
    // averages the source pixels each destination pixel covers, so it stays smooth at any scale.
    void image(float x, float y, float size, const std::uint32_t *source, int source_size);
    // A line of `thickness` logical units: an axis-aligned filled rectangle without rounding.
    void rect(float x, float y, float w, float h, Color c, float alpha);
    // Shortens `str` with an ellipsis to fit `max_width`.
    [[nodiscard]] std::string fit(const std::string &str, float size, float max_width) const;
    // Greedy word wrap.
    [[nodiscard]] std::vector<std::string> wrap(const std::string &str, float size, float max_width) const;

private:
    struct Glyph {
        int x0, y0, w, h;
        std::vector<unsigned char> bitmap;
    };
    // The font, glyph and scale that draw `cp` at `pixel_size`: the main font, else the Japanese one, else '?'.
    struct Source {
        const stbtt_fontinfo *font;
        int index;
        float scale;
    };
    [[nodiscard]] Source source(std::uint32_t cp, float pixel_size) const;
    const Glyph &glyph(std::uint32_t cp, float pixel_size, float subx);
    [[nodiscard]] float advance(std::uint32_t cp, float pixel_size) const;
    void blend(int x, int y, Color c, float a);

    std::vector<unsigned char> font_data_;
    std::unique_ptr<stbtt_fontinfo> font_;
    bool font_ok_ = false;
    std::vector<unsigned char> japanese_data_;
    std::unique_ptr<stbtt_fontinfo> japanese_; // null when the file is missing
    std::map<std::tuple<std::uint32_t, int, int>, Glyph> glyphs_; // codepoint, pixel size * 4, subpixel quarter

    std::vector<std::uint32_t> *px_ = nullptr;
    int width_ = 0, height_ = 0;
    float scale_ = 1.0f;
};

} // namespace mhf::overlay
