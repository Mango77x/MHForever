#pragma once

// Texture filtering policy for the OpenGL backend (Auto / Linear / Nearest).
// Kept free of SDL/OpenGL headers so game hosts, the frontend header, and unit tests can include it directly.

#include <cstdint>
#include <optional>
#include <string_view>

namespace mhf::psp {

// Global texture filtering mode chosen by the player:
//   Auto:    respects the PSP GE kTexFilter register per draw.
//   Linear:  forces bilinear filtering on 3D textures; leaves unresized 2D as drawn.
//   Nearest: forces nearest-neighbor filtering on 3D and 2D game textures, while preserving smooth HD text and resized HUD.
enum class TextureFilter : int {
    Auto = 0,
    Linear = 1,
    Nearest = 2,
};

constexpr std::string_view texture_filter_name(TextureFilter filter) noexcept {
    switch (filter) {
    case TextureFilter::Linear: return "linear";
    case TextureFilter::Nearest: return "nearest";
    case TextureFilter::Auto:
    default: return "auto";
    }
}

constexpr std::optional<TextureFilter> parse_texture_filter(std::string_view text) noexcept {
    if (text == "auto") return TextureFilter::Auto;
    if (text == "linear") return TextureFilter::Linear;
    if (text == "nearest") return TextureFilter::Nearest;
    return std::nullopt;
}

struct TextureFilterContext {
    TextureFilter mode{TextureFilter::Auto};
    bool game_linear{false};   // PSP GE kTexFilter magnification bit (bit 8)
    bool through{false};       // 2D through-mode draw
    bool resized{false};       // 2D draw shrunk by HUD scale or narrowed by widescreen
    bool hd_text{false};       // HD glyph page or built-in HD text texture
    bool has_mips{false};      // texture has mipmap levels (HD text or mipmapped pack image)
    std::uint8_t pack_filter{0}; // texture pack [filtering] override: 0 auto/none, 1 nearest, 2 linear
};

// Resolves whether a draw uses linear (true) or nearest (false) texture filtering.
constexpr bool resolve_texture_linear(const TextureFilterContext &ctx) noexcept {
    if (ctx.pack_filter != 0) return ctx.pack_filter == 2;
    if (ctx.resized) return true;
    switch (ctx.mode) {
    case TextureFilter::Linear: return ctx.game_linear || !ctx.through;
    case TextureFilter::Nearest: return ctx.hd_text ? (ctx.game_linear || ctx.has_mips) : false;
    case TextureFilter::Auto:
    default: return ctx.game_linear;
    }
}

struct TextureSampling {
    bool linear{false};          // GL_LINEAR vs GL_NEAREST magnification (and non-mip minification)
    bool mip_linear_blend{true}; // GL_*_MIPMAP_LINEAR (true) vs GL_*_MIPMAP_NEAREST (false) when mipmapped
    bool anisotropy{true};       // apply configured texture-pack anisotropy when mipmapped
};

// Resolves full sampling policy. Auto and Linear preserve trilinear mip blending (GL_NEAREST_MIPMAP_LINEAR when
// nearest) and configured anisotropy for mipmapped pack textures; explicit Nearest disables both on nearest draws.
constexpr TextureSampling resolve_texture_sampling(const TextureFilterContext &ctx) noexcept {
    const bool linear = resolve_texture_linear(ctx);
    const bool strict_nearest = !linear && ctx.mode == TextureFilter::Nearest;
    return {linear, !strict_nearest, !strict_nearest};
}

} // namespace mhf::psp
