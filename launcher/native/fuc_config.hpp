#pragma once

// FUComplete's options for a FUComplete copy (games/mhfu/regions/ULJM05500-FUC). FUComplete reads them at every start
// from CONFIG.BIN in its own save folder, PSP/SAVEDATA/FUCDAT on the memory stick, which it fills from the disc's FUC
// folder the first time. The launcher fills that folder the same way when it is missing and writes the player's choices
// into CONFIG.BIN before each start, so the options are set in the launcher like the rest.
//
// CONFIG.BIN (64 bytes): a version string, then one byte per option from offset 0x10 (FUCTool's res/config.json lists
// them). The minimap size is always left at its default: MHForever's own HUD size scales the whole quest HUD instead.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mhf::launcher::fuc {

struct Options {
    int save_region = 1;      // the saves FUComplete loads: 0 Japanese, 1 North American, 2 European
    bool true_raw = false;    // weapons show their true raw, element and status values
    bool dos_audio = false;   // some battle themes replaced with their Monster Hunter Dos counterparts
    int fov = 0;              // 0 default, 1 minor, 2 moderate, 3 extreme
    int camera = 3;           // starting vertical camera position in quests: 0 lowest .. 3 default .. 4 highest
    bool horn_tweak = false;  // FUComplete's hunting horn changes (notes, motion values)
    bool lao_early_kill = false; // Lao-Shan Lung, Ash Lao-Shan Lung and Shen Gaoren on Fortress can die before area 5
    bool full_chest = false;  // equipment can be changed at the Guildhall item chest (offline)
    bool drink_buff = false;  // a health and stamina boost at the Guildhall table, by the number of Felyne chefs
};

// The names the launcher's settings use for the save regions and the fields of view, in the order of their values.
inline constexpr std::array<const char *, 3> kSaveRegions = {"ULJM05500", "ULUS10391", "ULES01213"};
inline constexpr std::array<const char *, 4> kFieldsOfView = {"default", "minor", "moderate", "extreme"};

inline constexpr std::size_t kConfigSize = 0x1C; // the bytes up to the last option this file knows

// Writes `options` into CONFIG.BIN's bytes (the minimap at its default). False when `config` is too short to be one.
bool apply(std::vector<std::uint8_t> &config, const Options &options);

// The folder FUComplete keeps its files in on the memory stick `stick`.
std::filesystem::path save_folder(const std::filesystem::path &stick);

// Fills <stick>/PSP/SAVEDATA/FUCDAT from the disc image's FUC folder when it has no CONFIG.BIN yet, then writes `options`
// into CONFIG.BIN. Returns an empty string, or what went wrong in words for the player.
std::string prepare(const std::filesystem::path &iso, const std::filesystem::path &stick, const Options &options);

} // namespace mhf::launcher::fuc
