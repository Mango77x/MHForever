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
    bool indicators = false, automatic_tools = false, spanish = false;
    std::function<bool()> input_blocked; // frontend menu/dialog input and its release latch
    void update(psp::Runtime &rt, psp::Context &ctx);
    std::string hint(psp::Runtime &rt) const;
    bool load_icons(const std::filesystem::path &folder);
    std::vector<overlay::HintSprite> sprites(psp::Runtime &rt) const;

private:
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
