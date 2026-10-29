#include "overlay/painter.hpp"

#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace mhf::overlay {

namespace {

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

// Premultiplied "over" in 8-bit integers: dst = src * a + dst * (1 - a), alpha included.
void blend_px(std::uint32_t &d, std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    if (a == 0) return;
    const std::uint32_t inv = 255u - a;
    const auto mix = [&](std::uint32_t src, unsigned shift) {
        const std::uint32_t v = (src * a + ((d >> shift) & 0xFFu) * inv + 127u) / 255u;
        return std::min(v, 255u) << shift;
    };
    d = mix(r, 0) | mix(g, 8) | mix(b, 16) | mix(255u, 24);
}

std::uint32_t byte(float v) { return static_cast<std::uint32_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f)); }

} // namespace

Painter::Painter(const std::filesystem::path &font_directory) : font_(std::make_unique<stbtt_fontinfo>()) {
    std::ifstream f(font_directory / "LiberationSans-Regular.ttf", std::ios::binary);
    font_data_.assign(std::istreambuf_iterator<char>(f), {});
    font_ok_ = !font_data_.empty() &&
               stbtt_InitFont(font_.get(), font_data_.data(), stbtt_GetFontOffsetForIndex(font_data_.data(), 0)) != 0;
    if (!font_ok_) std::fprintf(stderr, "[overlay] no font in %s\n", font_directory.string().c_str());
    std::ifstream j(font_directory / "NotoSansJP-Regular.otf", std::ios::binary);
    japanese_data_.assign(std::istreambuf_iterator<char>(j), {});
    if (!japanese_data_.empty()) {
        japanese_ = std::make_unique<stbtt_fontinfo>();
        if (stbtt_InitFont(japanese_.get(), japanese_data_.data(), stbtt_GetFontOffsetForIndex(japanese_data_.data(), 0)) == 0)
            japanese_.reset();
    }
}

Painter::~Painter() = default;

void Painter::begin(std::vector<std::uint32_t> &pixels, int width, int height, float scale) {
    width_ = width;
    height_ = height;
    scale_ = scale;
    pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0u);
    px_ = &pixels;
}

void Painter::blend(int x, int y, Color c, float a) {
    if (a <= 0.0f || x < 0 || y < 0 || x >= width_ || y >= height_) return;
    blend_px((*px_)[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)], byte(c.r),
             byte(c.g), byte(c.b), byte(std::min(a, 1.0f)));
}

// Only the corners need the distance function; straight edges and the interior take fast paths.
void Painter::rrect(float x, float y, float w, float h, float radius, Color top, float alpha, float border,
                    const Color *bottom) {
    const float s = scale_;
    const float X = x * s, Y = y * s, Wd = w * s, Hd = h * s, R = radius * s, B = border * s;
    const int x0 = std::max(0, static_cast<int>(std::floor(X))), x1 = std::min(width_, static_cast<int>(std::ceil(X + Wd)));
    const int y0 = std::max(0, static_cast<int>(std::floor(Y))), y1 = std::min(height_, static_cast<int>(std::ceil(Y + Hd)));
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
        std::uint32_t *row = px_->data() + static_cast<std::size_t>(py) * static_cast<std::size_t>(width_);
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

void Painter::fill_all(Color c, float alpha) {
    const std::uint32_t a = byte(alpha);
    const std::uint32_t value = ((byte(c.r) * a / 255u)) | ((byte(c.g) * a / 255u) << 8u) |
                                ((byte(c.b) * a / 255u) << 16u) | (a << 24u);
    for (auto &d : *px_) {
        if (d == 0) d = value;
        else blend_px(d, byte(c.r), byte(c.g), byte(c.b), a);
    }
}

Painter::Source Painter::source(std::uint32_t cp, float pixel_size) const {
    const float scale = stbtt_ScaleForPixelHeight(font_.get(), pixel_size);
    if (const int gi = stbtt_FindGlyphIndex(font_.get(), static_cast<int>(cp)); gi != 0 || cp == ' ') return {font_.get(), gi, scale};
    if (japanese_) {
        if (const int gi = stbtt_FindGlyphIndex(japanese_.get(), static_cast<int>(cp)); gi != 0) {
            // The same em as the main font (the two fonts' ascent-to-descent heights differ).
            const float em = scale / stbtt_ScaleForMappingEmToPixels(font_.get(), 1.0f);
            return {japanese_.get(), gi, stbtt_ScaleForMappingEmToPixels(japanese_.get(), em)};
        }
    }
    return {font_.get(), stbtt_FindGlyphIndex(font_.get(), '?'), scale};
}

const Painter::Glyph &Painter::glyph(std::uint32_t cp, float pixel_size, float subx) {
    const int size_key = static_cast<int>(pixel_size * 4.0f + 0.5f);
    const int sub_key = static_cast<int>(subx * 4.0f) & 3;
    const auto key = std::make_tuple(cp, size_key, sub_key);
    if (auto it = glyphs_.find(key); it != glyphs_.end()) return it->second;
    Glyph g{};
    const auto [font, gi, scale] = source(cp, static_cast<float>(size_key) / 4.0f);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(font, gi, scale, scale, static_cast<float>(sub_key) / 4.0f, 0.0f, &x0, &y0,
                                    &x1, &y1);
    g.x0 = x0;
    g.y0 = y0;
    g.w = std::max(0, x1 - x0);
    g.h = std::max(0, y1 - y0);
    g.bitmap.resize(static_cast<std::size_t>(g.w) * static_cast<std::size_t>(g.h));
    if (g.w > 0 && g.h > 0)
        stbtt_MakeGlyphBitmapSubpixel(font, g.bitmap.data(), g.w, g.h, g.w, scale, scale,
                                      static_cast<float>(sub_key) / 4.0f, 0.0f, gi);
    return glyphs_.emplace(key, std::move(g)).first->second;
}

float Painter::advance(std::uint32_t cp, float pixel_size) const {
    const auto [font, gi, scale] = source(cp, pixel_size);
    int adv, lsb;
    stbtt_GetGlyphHMetrics(font, gi, &adv, &lsb);
    return static_cast<float>(adv) * scale;
}

float Painter::measure(const std::string &text, float size, bool bold, float tracking) const {
    float w = 0.0f;
    int glyphs = 0;
    for (std::size_t i = 0; i < text.size(); ++glyphs) w += advance(next_codepoint(text, i), size * scale_);
    return w / scale_ + (bold ? 0.4f : 0.0f) + tracking * static_cast<float>(glyphs);
}

void Painter::image(float x, float y, float size, const std::uint32_t *source, int source_size) {
    const int dest = std::max(1, static_cast<int>(std::lround(size * scale_)));
    const int x0 = static_cast<int>(std::lround(x * scale_)), y0 = static_cast<int>(std::lround(y * scale_));
    const float ratio = static_cast<float>(source_size) / static_cast<float>(dest);
    for (int dy = 0; dy < dest; ++dy) {
        const int py = y0 + dy;
        if (py < 0 || py >= height_) continue;
        const int sy0 = static_cast<int>(static_cast<float>(dy) * ratio);
        const int sy1 = std::max(sy0 + 1, static_cast<int>(static_cast<float>(dy + 1) * ratio));
        for (int dx = 0; dx < dest; ++dx) {
            const int pxx = x0 + dx;
            if (pxx < 0 || pxx >= width_) continue;
            const int sx0 = static_cast<int>(static_cast<float>(dx) * ratio);
            const int sx1 = std::max(sx0 + 1, static_cast<int>(static_cast<float>(dx + 1) * ratio));
            std::uint32_t sum[4] = {};
            int count = 0;
            for (int sy = sy0; sy < std::min(sy1, source_size); ++sy) {
                for (int sx = sx0; sx < std::min(sx1, source_size); ++sx) {
                    const std::uint32_t p = source[sy * source_size + sx];
                    for (int c = 0; c < 4; ++c) sum[c] += (p >> (8 * c)) & 0xFFu;
                    ++count;
                }
            }
            if (count == 0) continue;
            const std::uint32_t a = sum[3] / static_cast<std::uint32_t>(count);
            if (a == 0) continue;
            // Premultiplied source: dst = src + dst * (1 - a).
            std::uint32_t &d = (*px_)[static_cast<std::size_t>(py) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(pxx)];
            const std::uint32_t inv = 255u - a;
            std::uint32_t out = 0;
            for (int c = 0; c < 4; ++c) {
                const std::uint32_t v = sum[c] / static_cast<std::uint32_t>(count) + (((d >> (8 * c)) & 0xFFu) * inv + 127u) / 255u;
                out |= std::min(v, 255u) << (8 * c);
            }
            d = out;
        }
    }
}

void Painter::rect(float x, float y, float w, float h, Color c, float alpha) {
    const int x0 = std::max(0, static_cast<int>(std::lround(x * scale_))), x1 = std::min(width_, static_cast<int>(std::lround((x + w) * scale_)));
    const int y0 = std::max(0, static_cast<int>(std::lround(y * scale_))), y1 = std::min(height_, static_cast<int>(std::lround((y + h) * scale_)));
    const std::uint32_t a = byte(std::min(alpha, 1.0f)), r = byte(c.r), g = byte(c.g), b = byte(c.b);
    for (int py = y0; py < y1; ++py) {
        std::uint32_t *row = px_->data() + static_cast<std::size_t>(py) * static_cast<std::size_t>(width_);
        for (int pxx = x0; pxx < x1; ++pxx) blend_px(row[pxx], r, g, b, a);
    }
}

float Painter::text(float x, float baseline, const std::string &str, float size, Color c, float alpha, bool bold,
                    float tracking) {
    if (!font_ok_) return 0.0f;
    const float s = scale_;
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
            pen += advance(cp, pixel_size) + tracking * s;
        }
    }
    return measure(str, size, bold, tracking);
}

std::string Painter::fit(const std::string &str, float size, float max_width) const {
    if (measure(str, size) <= max_width) return str;
    std::string out = str;
    while (!out.empty() && measure(out + "\xE2\x80\xA6", size) > max_width) {
        out.pop_back();
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
    }
    return out + "\xE2\x80\xA6";
}

std::vector<std::string> Painter::wrap(const std::string &str, float size, float max_width) const {
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
            if (measure(candidate, size) <= max_width) {
                line = candidate;
            } else if (measure(word, size) <= max_width) {
                lines.push_back(line);
                line = word;
            } else {
                // Too wide for any line (Japanese has no spaces): break it between characters.
                if (!line.empty()) line += ' ';
                for (std::size_t k = 0; k < word.size();) {
                    const std::size_t from = k;
                    next_codepoint(word, k);
                    const std::string ch = word.substr(from, k - from);
                    if (!line.empty() && measure(line + ch, size) > max_width) {
                        lines.push_back(line);
                        line.clear();
                    }
                    line += ch;
                }
            }
            i = j + 1;
        }
        lines.push_back(line);
        start = end + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

} // namespace mhf::overlay
