#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace mhf::mhfu {
// Whole files of the game's DATA.BIN, read from the player's own image with the player's own prepared executable (the
// existing game-specific reader, tools/data_bin.hpp). Nothing when the image, the executable or a file is missing, or a
// file is larger than `limit`.
std::optional<std::vector<std::vector<std::uint8_t>>> read_game_files(const std::filesystem::path &iso,
                                                                      const std::filesystem::path &executable,
                                                                      std::span<const std::uint32_t> ids,
                                                                      std::uint32_t limit = 1024 * 1024);
} // namespace mhf::mhfu
