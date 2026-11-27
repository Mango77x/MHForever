#pragma once

// Button prompts in another controller's style (enhancement, off by default).
//
// The game draws its button prompts as small pieces of a few textures of its own (the PSP's circle, cross, triangle and
// square on a dark disc, the L and R plates, START and SELECT). With a style other than the original, the GL frontend
// paints over those pieces in its own copy of the texture, as it uploads it: the game's memory, its texture and its
// draws are left as they are, and turning the style off uploads the game's texture again.
//
// Nothing drawn here is shipped as an image or comes from a font:
// - the disc under a face button is the game's own, from the player's copy: the four face pieces differ only in their
//   symbol, so the texels they agree on are the disc without one;
// - the plates are the game's own too, with their lettering painted over by a row of the same plate that has none;
// - the letters (A, B, X, Y, BACK, and LB or RB for a plate whose button is bound to that bumper alone) are a few
//   strokes each, lines and arcs worked out here, in the game's own lettering colour for the plates.
//
// Which textures, and where the pieces are in them, is what the game module knows (Atlas); a texture is told by a few
// of its bytes, hashed from the player's own copy when the game starts (mark_of), so nothing of the game is kept here.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace mhf::psp::prompts {

enum class Style : int { Original = 0, Xbox = 1 };

// What a piece shows on the PSP.
enum class Icon : std::uint8_t { Circle, Cross, Triangle, Square, L, R, Start, Select };

struct Box {
    std::uint16_t x{}, y{}, w{}, h{}; // texels of the texture at the PSP's size
};

struct Cell {
    Icon icon{};
    Box box;                 // the piece
    Box text;                // plates: where its lettering is
    std::uint16_t clean_row{}; // plates: a row of the plate, across `text`, without lettering
};

struct Atlas {
    std::uint16_t width{}, height{};          // the texture's size
    std::uint32_t mark_offset{}, mark_size{}; // the bytes of the texture, as the game stores it, that tell it apart
    std::uint64_t mark{};                     // mark_of those bytes in the player's copy
    std::vector<Cell> cells;
};

[[nodiscard]] std::uint64_t mark_of(std::span<const std::uint8_t> bytes) noexcept;

// What the pieces are relabelled as. The face buttons, by Icon (Circle, Cross, Triangle, Square): where the buttons sit
// on the pad. The L and R plates: nothing (the game's own plate stays) unless the pad's binding of that button is one
// bumper alone. By default L and R are bound to a bumper and a trigger together, and no one name is right.
struct Labels {
    char face[4]{'B', 'A', 'Y', 'X'};
    const char *left = nullptr, *right = nullptr;
};

// The name of a pad binding (core/host/padmap.hpp: "left_shoulder", "left_shoulder+left_trigger", ...) on a plate:
// "LB" or "RB" for that bumper alone, nothing for any other binding.
[[nodiscard]] const char *shoulder_label(std::string_view binding) noexcept;

// Paints the pieces of `atlas` in `pixels` (ABGR8888, red in the low byte, as Ge::decode_texture gives them), a texture
// `scale` times the atlas's size (1: the game's own; more: a texture pack's image of it). False, with nothing changed,
// when the style is the original or the image is not that size.
bool repaint(const Atlas &atlas, Style style, std::span<std::uint32_t> pixels, unsigned scale, const Labels &labels = {});

} // namespace mhf::psp::prompts
