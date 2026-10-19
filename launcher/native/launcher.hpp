#pragma once

// The launcher's state and actions, independent of the UI toolkit: the library of the player's copies, settings,
// local identity, play statistics, and launching the game. Persisted in launcher.json next to the executable.

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

    // Everything the UI shows, as the bridge's State object.
    Json state();

    void set_settings(const Json &partial);
    void set_pseudonym(const std::string &name);

    // Identifies a disc image (PARAM.SFO + SHA-1 against known good dumps) and adds it to the library.
    // Returns the game object for the UI, or an error message. Thread-safe; slow (hashes the whole image).
    struct AddResult {
        std::optional<Json> game;
        std::string error;
    };
    AddResult add_game(const std::filesystem::path &iso, const Progress &progress);
    void remove_game(const std::string &id); // also deletes what the installer stored in the data directory

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
    install::Decryptor decryptor_;
};

} // namespace mhf::launcher
