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

    // Settings belong to a copy of a game (Home's "Game settings": video, audio, controls), except `copyImage`, which is the
    // launcher's own. set_settings() changes the copy chosen in the Library; state()["settings"] shows the same copy's values.
    void set_settings(const Json &partial);
    // The bindings the player changed in the game's own menu: the game writes them to controls_file() as it goes, and
    // this folds them into the settings (validated like any other change) and removes the file. Called when the launcher
    // starts, before a launch and when the game ends.
    void import_game_controls();
    [[nodiscard]] std::filesystem::path controls_file() const { return data_dir_ / "game_controls.txt"; }
    void set_pseudonym(const std::string &name);
    // Friends (docs/14-rooms-and-friends.md section 4): the UI keeps the list and replaces it whole; what is not a friend
    // (a 64-digit public key, a name) is dropped. `blocked` holds the keys of players whose requests are ignored.
    void set_friends(const Json &friends, const Json &blocked);
    // Friends in the game's Online tab while it runs: the launcher writes who is where and the invites to
    // game_friends_file() (the UI calls write_game_friends with {kind: "friend" | "invite", key, name, state: "offline" |
    // "launcher" | "playing", server, room}; anything else is dropped), and the game writes what the player did there
    // ("invite <key>", "dismiss <key>") to friend_actions_file(), which take_friend_actions() empties.
    [[nodiscard]] std::filesystem::path game_friends_file() const { return data_dir_ / "game_friends.txt"; }
    [[nodiscard]] std::filesystem::path friend_actions_file() const { return data_dir_ / "game_friend_actions.txt"; }
    void write_game_friends(const Json &entries);
    std::vector<std::pair<std::string, std::string>> take_friend_actions();

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
    // Where the saves live: the folder the player chose (Settings > Storage), else the default one given to the constructor.
    [[nodiscard]] const std::filesystem::path &saves_dir() const { return saves_dir_; }
    // Changes it. Every game's saves are copied into the new folder, except a game that already has saves there, whose
    // saves are left as they are; nothing is moved or deleted. An empty `folder` goes back to the default.
    struct SavesFolderChange {
        bool changed = false;
        int copied = 0; // games whose saves were copied
        std::string error;
    };
    SavesFolderChange set_saves_folder(const std::filesystem::path &folder);
    // Texture packs in PPSSPP's format (platforms/psp/frontend/texture_pack.hpp). review_texture_pack() finds the pack
    // in a folder the player chose (itself, a MAIN inside it, PPSSPP's PSP/TEXTURES/<id>) and says, per release in the
    // catalog, which ini it uses, how many textures it names and how many of their images are missing; nothing changes.
    [[nodiscard]] Json review_texture_pack(const std::filesystem::path &chosen) const;
    // Uses the pack in `folder` (a pack's own folder, as the review found it) where it is; empty forgets the pack, and
    // deletes it when it is the copy in the data folder. False (nothing changed) when there is no pack there.
    bool set_texture_pack(const std::filesystem::path &folder);
    void set_texture_pack_enabled(bool enabled);
    // Copies the pack in `folder` into the data folder and uses it from there. The copy goes to a hidden folder first:
    // the pack in use stays until the copy is complete, and a copy already there is kept in textures/.backup/<time>.
    // `progress` gets the share done and the file being copied, and stops the copy by returning false.
    bool install_texture_pack(const std::filesystem::path &folder, const std::function<bool(double, const std::string &)> &progress,
                              std::string &error);
    [[nodiscard]] std::filesystem::path installed_texture_pack() const { return data_dir_ / "textures" / "pack"; }

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
    void apply_settings(const std::string &disc_id, const Json &partial); // mutex_ held; "" = no copy yet
    std::filesystem::path find_executable(const CatalogEntry &entry);
    const CatalogEntry *entry_for(const std::string &disc_id) const;
    std::string nostr_key() const;

    std::filesystem::path base_;
    std::filesystem::path file_;
    std::mutex mutex_;
    Json data_;
    std::string launched_disc_; // the copy plan_launch() last prepared: where the game's own control changes belong
    std::filesystem::path data_dir_;
    std::filesystem::path default_saves_dir_;
    std::filesystem::path saves_dir_;
    install::Decryptor decryptor_;
};

} // namespace mhf::launcher
