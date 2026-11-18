#pragma once

#include "gathering_art.hpp"
#include <span>
#include <filesystem>

namespace mhf::psp::textures {
class Pack;
}

namespace mhf::mhfu {
// Original swizzled HUD/items TMH atlases, read from the player's own game.
std::optional<GatheringArt> decode_game_gathering_art(std::span<const std::uint8_t> hud,
                                                      std::span<const std::uint8_t> items,
                                                      const psp::textures::Pack *pack = nullptr,
                                                      unsigned *replaced = nullptr);
std::optional<GatheringArt> load_game_gathering_art(const std::filesystem::path &iso,
                                                    const std::filesystem::path &executable, std::uint32_t hud_file,
                                                    std::uint32_t items_file, const psp::textures::Pack *pack = nullptr,
                                                    unsigned *replaced = nullptr);
} // namespace mhf::mhfu
