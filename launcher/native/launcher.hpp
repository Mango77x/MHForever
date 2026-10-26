#pragma once

// The launcher's state and actions, independent of the UI toolkit: the library of the player's copies, settings,
// local identity, play statistics, and launching the game. Persisted in launcher.json in the per-user data folder.

#include "install/executable_preparation.hpp"
#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mhf::launcher {

// One region of one game, from games/<id>/game.toml and regions/<disc>/region.toml (identification data only).
struct CatalogEntry {
    const char *game;
    const char *title;
    const char *disc_id;
    const char *version;
    const char *market;
    const char *status; // "planned" regions are recognised but not playable
    struct Dump {
        std::uint64_t size;
        const char *sha1;
    };
    std::vector<Dump> dumps;
    std::uint64_t dev_executable_size; // the prepared (plain) executable the recompiled code was generated from
    const char *dev_executable_sha1;
    std::uint64_t encrypted_executable_size; // PSP_GAME/SYSDIR/EBOOT.BIN on the disc; the installer accepts only this
    const char *encrypted_executable_sha1;
};

const std::vector<CatalogEntry> &catalog();

struct Progress {
    std::function<void(const char *phase, double fraction)> report;
};

class Launcher {
public:
    explicit Launcher(std::filesystem::path base_dir);
    // `data_dir` holds what the installer makes from the player's disc image (install/data_dir.hpp).
    Launcher(std::filesystem::path base_dir, std::filesystem::path data_dir);
    // `saves_dir` holds one memory stick per game, <saves_dir>/<DISC_ID> (install::user_saves_dir() in the real launcher).
    Launcher(std::filesystem::path base_dir, std::filesystem::path data_dir, std::filesystem::path saves_dir);

    // Everything the UI shows, as the bridge's State object.
    Json state();

    void set_settings(const Json &partial);
    // The bindings the player changed in the game's own menu: the game writes them to controls_file() as it goes, and
    // this folds them into the settings (validated like any other change) and removes the file. Called when the launcher
    // starts, before a launch and when the game ends.
    void import_game_controls();
    [[nodiscard]] std::filesystem::path controls_file() const { return data_dir_ / "game_controls.txt"; }
    void set_pseudonym(const std::string &name);

    // Identifies a disc image (PARAM.SFO + SHA-1 against known good dumps) and adds it to the library.
    // Returns the game object for the UI, or an error message. Thread-safe; slow (hashes the whole image).
    struct AddResult {
        std::optional<Json> game;
        std::string error;
    };
    AddResult add_game(const std::filesystem::path &iso, const Progress &progress);
    // `id` is a disc id (one copy) or a game id (every copy of that game); also deletes what the installer stored.
    void remove_game(const std::string &id);
    // Which copy of a game Play, Settings and the saves use when the library holds several regions. False if the library
    // has no such supported copy.
    bool select_game(const std::string &disc_id);

    // Replaces the executable decryption this launcher uses (tests; the default is install::builtin_decryptor()).
    void set_decryptor(install::Decryptor decryptor) { decryptor_ = std::move(decryptor); }

    // Developer builds need the prepared executable. Returns false if `path` does not match.
    bool set_executable(const std::filesystem::path &path);

    // Everything needed to start the game, or why it can't start.
    struct LaunchPlan {
        std::filesystem::path program;
        std::vector<std::string> args;
        std::filesystem::path cwd;
        std::filesystem::path log;
        std::string error;
    };
    LaunchPlan plan_launch(const std::string &game_id);
    void record_session(std::int64_t seconds);

    Json identity_backup();
    [[nodiscard]] const std::filesystem::path &base_dir() const { return base_; }
    [[nodiscard]] const std::filesystem::path &data_dir() const { return data_dir_; }
    [[nodiscard]] const std::filesystem::path &saves_dir() const { return saves_dir_; }

    // Saves used to live in <launcher folder>/memstick, which an update (a new folder) leaves behind. Copies what is
    // there into the memory stick of `disc_id` once per source folder, never overwriting and never deleting the old files.
    void import_legacy_saves(const std::string &disc_id);

    // Importing saves made by a PSP or by PPSSPP (the data files are encrypted; the game decrypts them when it loads
    // them, see docs/05-runtime-hle.md section 5d). `scan_saves` looks in `source` (a memory stick folder, its
    // PSP/SAVEDATA folder, or a save folder itself) for the saves of `disc_id`; saves of another version of the game are
    // only mentioned in `note`, as their key differs and the game could not read them.
    struct SaveCandidate {
        std::string folder; // e.g. ULES01213
        std::filesystem::path path;
        std::string title;
        bool exists = false; // the player already has a save with that folder name
    };
    struct SaveScan {
        std::vector<SaveCandidate> found;
        std::string note; // why nothing was found, when it can be said
    };
    SaveScan scan_saves(const std::string &disc_id, const std::filesystem::path &source) const;
    // Copies the chosen saves into the memory stick of `disc_id`. A save the player already has with the same folder name
    // is moved to <saves>/<disc id>/backups/<date and time>/ first, never deleted. Nothing is left half done.
    struct SaveImport {
        int imported = 0;
        std::filesystem::path backup; // where the replaced saves went (empty when none was)
        std::string error;
    };
    SaveImport import_saves(const std::string &disc_id, const std::vector<SaveCandidate> &chosen);
    // The disc id of the player's copy of `game_id` (empty when there is none).
    std::string disc_id_for(const std::string &game_id);

private:
    void load();
    void save();
    std::filesystem::path find_executable(const CatalogEntry &entry);
    const CatalogEntry *entry_for(const std::string &disc_id) const;
    std::string friend_code() const;

    std::filesystem::path base_;
    std::filesystem::path file_;
    std::mutex mutex_;
    Json data_;
    std::filesystem::path data_dir_;
    std::filesystem::path saves_dir_;
    install::Decryptor decryptor_;
};

} // namespace mhf::launcher
