#pragma once

// Where installed game data lives (docs/07-launcher-ux.md §7). Everything the installer produces from the player's
// disc image goes here, never into the repository or next to the executable.

#include <filesystem>
#include <string_view>

namespace mhf::launcher::install {

// MHF_DATA_DIR if set; otherwise %APPDATA%/MHForever on Windows, $XDG_DATA_HOME/MHForever or
// ~/.local/share/MHForever elsewhere.
std::filesystem::path user_data_dir();

// <data>/games/<DISC_ID>/ and the files the installer writes into it.
std::filesystem::path game_dir(const std::filesystem::path &data_dir, std::string_view disc_id);
std::filesystem::path executable_path(const std::filesystem::path &data_dir, std::string_view disc_id);
std::filesystem::path image_path(const std::filesystem::path &data_dir, std::string_view disc_id);

} // namespace mhf::launcher::install
