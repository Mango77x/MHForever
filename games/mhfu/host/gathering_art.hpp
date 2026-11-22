#pragma once

#include "gathering_logic.hpp"
#include "overlay/song_panel.hpp"
#include <array>

namespace mhf::mhfu {
struct GatheringImage {
    int size{};
    std::shared_ptr<const std::vector<std::uint32_t>> pixels;
};
// Cropped atlas images: frame, Circle, pickaxe, net, question mark; and Circle as the Xbox button prompts show it
// (button_prompts.hpp), without pixels when it could not be made.
using GatheringArt = std::array<GatheringImage, 6>;
std::uint8_t gathering_alpha(std::uint8_t previous, bool active);
// `xbox_button`: the button as the Xbox prompts show it, when the art has it.
std::vector<overlay::HintSprite> gathering_sprites(const GatheringArt &art, GatheringKind kind, std::uint8_t alpha,
                                                   bool button, bool unavailable = false, bool xbox_button = false);
} // namespace mhf::mhfu
