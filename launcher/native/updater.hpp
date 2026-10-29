#pragma once

// Updates from the project's GitHub releases (docs/07-launcher-ux.md): find the latest release, download the archive for
// this system with the system's curl, check it against the SHA-256 GitHub publishes for it, unpack it with the system's
// tar and put its files in place of the running ones, keeping the replaced ones until the new launcher has started.

#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace mhf::launcher::update {

inline constexpr const char *kLatestRelease = "https://api.github.com/repos/Mango77x/MHForever/releases/latest";

struct Release {
    std::string version;      // "0.6.3"
    std::string page;         // the release page on GitHub
    std::string archive;      // the asset for this system
    std::string archive_url;
    std::uint64_t archive_size = 0;
    std::string sha256;       // lowercase hex, as GitHub computed it for the asset
};

// The archive name of this system: "windows-x64.zip" or "linux-x64.tar.gz" at its end.
std::string archive_suffix();
// True when `candidate` ("v0.6.3", "0.6.3") is a later version than `current` ("0.6.2").
bool newer(std::string_view candidate, std::string_view current);
// The release as GitHub's API describes it, with the asset whose name ends with `suffix`; nothing when the release has no
// such asset or GitHub gives no SHA-256 for it.
std::optional<Release> pick(const Json &release, std::string_view suffix);
// Lowercase hex SHA-256 of a file ("" when it cannot be read).
std::string sha256_of(const std::filesystem::path &file);

// Puts every file of `unpacked` in `install`, at the same relative path. A file it replaces is first renamed to
// <name>.old (Windows lets a running program be renamed, not overwritten); if anything fails, every file is put back as
// it was. Files of `install` the archive does not have are left alone. Returns an error message, empty on success.
std::string install(const std::filesystem::path &unpacked, const std::filesystem::path &install);
// Deletes the <name>.old files a previous update left in `install` (the running launcher can only do it once restarted).
void remove_old(const std::filesystem::path &install);

// Downloads `url` to `to` with the system's curl, calling `progress` with the fraction done when the size is known.
// Blocks. Returns an error message, empty on success.
using Progress = std::function<void(double)>;
std::string download(const std::string &url, const std::filesystem::path &to, std::uint64_t expected_size,
                     const Progress &progress, const std::filesystem::path &work);
// Unpacks a .zip or .tar.gz into `into` with the system's tar. Blocks. Returns an error message, empty on success.
std::string unpack(const std::filesystem::path &archive, const std::filesystem::path &into, const std::filesystem::path &work);

// The release to ask about: kLatestRelease, or MHF_UPDATE_URL when it is set (tests point it at a release described in a
// file:// URL, whose archive may then be a file:// URL too).
std::string latest_release_url();
// Asks for the latest release; nothing when there is none newer than `current`, or when it cannot be asked (`error`
// says why).
std::optional<Release> check(const std::filesystem::path &work, std::string_view current, std::string &error);
// Downloads `release`, checks its SHA-256, unpacks it and puts its files in `install`, reporting each phase
// ("downloading" with the fraction done, "verifying", "installing"). Nothing is changed unless every step works. Returns an
// error message, empty on success.
using Phase = std::function<void(const char *, double)>;
std::string apply(const Release &release, const std::filesystem::path &work, const std::filesystem::path &install,
                  const Phase &phase);

} // namespace mhf::launcher::update
