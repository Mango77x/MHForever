#pragma once

#include "button_prompts.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace mhf::mhfu {
// The game's textures that hold its button prompts (platforms/psp/frontend/button_prompts.hpp): the quest HUD's atlas
// (the second image of its file: the face buttons, L and R, START and SELECT) and the one the menus use (the fourth
// image of its file: the face buttons, L and R). Where the pieces are is the same in every language; which bytes tell
// each texture apart is worked out from the player's own copy.
//
// A file that is not what it should be gives no atlas: the four face pieces must be one disc with four different
// symbols, and the two plates of a pair one plate with different lettering. Either file may be missing.
std::vector<psp::prompts::Atlas> prompt_atlases(std::span<const std::uint8_t> hud_file, std::span<const std::uint8_t> menu_file);
std::vector<psp::prompts::Atlas> load_prompt_atlases(const std::filesystem::path &iso, const std::filesystem::path &executable,
                                                     std::optional<std::uint32_t> hud_file, std::optional<std::uint32_t> menu_file);
} // namespace mhf::mhfu
