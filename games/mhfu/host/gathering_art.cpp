#include "gathering_art.hpp"
#include <algorithm>

namespace mhf::mhfu {
std::uint8_t gathering_alpha(std::uint8_t previous, bool active) {
    if (active) return static_cast<std::uint8_t>(previous == 0 ? 15 : std::min(255, int(previous) + 64));
    return static_cast<std::uint8_t>(previous == 0 ? 0 : std::max(15, int(previous) - 64));
}
std::vector<overlay::HintSprite> gathering_sprites(const GatheringArt &art, GatheringKind kind, std::uint8_t alpha,
                                                   bool button, bool unavailable) {
    if (!alpha) return {};
    std::vector<overlay::HintSprite> sprites;
    auto add = [&](int image, float x, float y, float size, float opacity) {
        sprites.push_back({x, y, size, opacity, art[image].size, art[image].pixels});
    };
    add(0, 225, 80, 32, float(alpha) / 255.0f);
    if (alpha == 255 && kind != GatheringKind::none) {
        add(kind == GatheringKind::mining ? 2 : kind == GatheringKind::bugs ? 3 : 4, 229, 84, 24, 1);
        if (unavailable) {
            // Premultiplied sprite: a red X with a dark edge, generated without game
            // assets.
            static const auto cross = [] {
                auto pixels = std::make_shared<std::vector<std::uint32_t>>(32 * 32, 0);
                for (int y = 3; y < 29; ++y)
                    for (int x = 3; x < 29; ++x) {
                        const int distance = std::min(std::abs(x - y), std::abs(x + y - 31));
                        if (distance <= 3) (*pixels)[y * 32 + x] = distance <= 1 ? 0xff3030f0u : 0xff101018u;
                    }
                return std::shared_ptr<const std::vector<std::uint32_t>>(pixels);
            }();
            sprites.push_back({229, 84, 24, 1, 32, cross});
        } else if (button)
            add(1, 219, 100, 15, 1);
    }
    return sprites;
}
} // namespace mhf::mhfu
