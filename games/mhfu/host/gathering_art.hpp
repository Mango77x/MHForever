#pragma once

#include "gathering_logic.hpp"
#include "overlay/song_panel.hpp"
#include <array>
#include <filesystem>

namespace mhf::mhfu {
struct GatheringImage {
    int size{};
    std::shared_ptr<const std::vector<std::uint32_t>> pixels;
};
// Local images extracted from the player's atlas: frame, Circle, pickaxe, net,
// question mark.
using GatheringArt = std::array<GatheringImage, 5>;
std::optional<GatheringArt> load_gathering_art(const std::filesystem::path &folder);
std::uint8_t gathering_alpha(std::uint8_t previous, bool active);
std::vector<overlay::HintSprite> gathering_sprites(const GatheringArt &art, GatheringKind kind, std::uint8_t alpha,
                                                   bool button, bool unavailable = false);
} // namespace mhf::mhfu
