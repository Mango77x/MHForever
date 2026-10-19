#pragma once

// First-run setup from the player's own disc image (the launcher's "Add my copy" flow plus `launcher --install`):
// checks the image, prepares the game's executable from it, and optionally keeps a copy of the image in the per-user
// data directory. The launcher talks to the player; this talks to the files.

#include "install/executable_preparation.hpp"
#include "launcher.hpp"

#include <filesystem>
#include <string>

namespace mhf::launcher::install {

struct InstallOptions {
    bool prepare_executable = true; // false when a usable executable is already known (developer builds)
    bool copy_image = false;        // keep a copy of the image in the data directory instead of using it in place
};

enum class InstallError {
    None,
    NotAnImage,
    CompressedImage,
    UnknownRelease,
    ExecutableMissing,
    WrongRelease,
    PreparationUnavailable,
    PreparationFailed,
    NoSpace,
    Io,
};

struct InstallResult {
    InstallError error = InstallError::None;
    std::string message;           // plain language, ready to show the player
    std::filesystem::path image;   // the image the game will read: the copy, or `iso` itself
    std::filesystem::path executable; // the prepared executable, when one was prepared
};

// `entry` is the catalog entry the image was identified as. Writes only under `data_dir`; on failure nothing partial is
// left behind and the data directory is otherwise untouched.
InstallResult install_from_image(const std::filesystem::path &iso, const CatalogEntry &entry,
                                 const std::filesystem::path &data_dir, const InstallOptions &options,
                                 const Decryptor &decrypt, const Progress &progress);

} // namespace mhf::launcher::install
