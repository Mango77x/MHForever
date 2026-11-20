#pragma once

#include "gathering_art.hpp"
#include "mhf/psp/runtime.hpp"
#include <functional>
#include <string>

namespace mhf::mhfu {
// Opt-in gathering assistance for mapped releases. Inspired by Lunng's Gathering Nodes mod.
// Uses the game's own interaction routine; no injected MIPS or fixed scratch
// RAM.
class Gathering {
public:
    bool indicators = false, automatic_tools = false;
    int language = 1; // PSP system language: Japanese, English, French, Spanish, German, Italian
    std::function<bool()> input_blocked; // frontend menu/dialog input and its release latch
    void update(psp::Runtime &rt, psp::Context &ctx);
    std::string hint(psp::Runtime &rt) const;
    void set_icons(GatheringArt art) { art_ = std::move(art); }
    std::vector<overlay::HintSprite> sprites(psp::Runtime &rt) const;

private:
    struct Selection {
        std::uint32_t player;
        std::uint16_t previous, tool;
        std::uint8_t action;
    };
    std::optional<Selection> selection_;
    bool held_ = false, busy_ = false;
    std::uint32_t scratch_ = 0;
    std::uint64_t updated_ = 0;
    std::string hint_;
    std::optional<GatheringArt> art_;
    GatheringKind kind_ = GatheringKind::none;
    bool visual_active_ = false, button_ = false, unavailable_ = false;
    std::uint8_t alpha_ = 0;
};
} // namespace mhf::mhfu
