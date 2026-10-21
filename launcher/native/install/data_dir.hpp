#pragma once

// Where installed game data lives (docs/07-launcher-ux.md §7). Everything the installer produces from the player's
// disc image goes here, never into the repository or next to the executable.

#include <filesystem>
#include <string_view>

namespace mhf::launcher::install {

// MHF_DATA_DIR if set; otherwise %APPDATA%/MHForever on Windows, $XDG_DATA_HOME/MHForever or
// ~/.local/share/MHForever elsewhere.
std::filesystem::path user_data_dir();

// Where the player's saves live, outside the folder the launcher runs from so that updating the launcher (a new
// folder) never loses them: MHF_SAVES_DIR if set; otherwise <Documents>/MHForever/saves on Windows (the real Documents
// folder, wherever the system redirects it), <user data>/saves elsewhere. Each game has its own memory stick inside it,
// <saves>/<DISC_ID>/ (so titles and regions never mix); that is what the game gets as its memory stick root.
std::filesystem::path user_saves_dir();
std::filesystem::path game_saves_dir(const std::filesystem::path &saves_dir, std::string_view disc_id);

// <data>/games/<DISC_ID>/ and the files the installer writes into it.
std::filesystem::path game_dir(const std::filesystem::path &data_dir, std::string_view disc_id);
std::filesystem::path executable_path(const std::filesystem::path &data_dir, std::string_view disc_id);
std::filesystem::path image_path(const std::filesystem::path &data_dir, std::string_view disc_id);

} // namespace mhf::launcher::install
