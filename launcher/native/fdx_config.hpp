#pragma once

// Freedom DX's options for a Freedom DX copy (games/mhf1/regions/ULES00318-FDX). Freedom DX reads them at every start
// from CONFIG.BIN in PSP/SAVEDATA/FDXDAT on the memory stick (with its event quests, EVENT.BIN, which come with the
// player's download). The launcher writes the player's choices into CONFIG.BIN before each start, so the options are
// set in the launcher like the rest; without a CONFIG.BIN yet it makes one.
//
// CONFIG.BIN (48 bytes): a version string, then one byte per option from offset 0x10, as Freedom DX's code reads them
// (source/ULES00318: Init.asm, main.asm, CatSkills.asm, DrinkBuff.asm). The minimap scale is always left at 100 %:
// MHForever's own HUD size scales the whole quest HUD instead.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mhf::launcher::fdx {

struct Options {
    bool hold_to_gather = false;    // keep the gather button held while crouched
    bool true_raw = false;          // weapons show their true raw attack
    bool lao_early_kill = false;    // Lao-Shan Lung can die before the last area
    bool sns_debuff = false;        // Sword & Shield sharpness and damage multiplier 1.2 instead of 1.5, as in Dos
    bool file_replacer = false;     // files from FDXDAT/NATIVEPSP/<game id>/ replace the game's
    bool felyne_skills = false;     // each Felyne's skill set is shown
    bool drink_buff = false;        // a health and stamina boost at the Gathering Hall table, by the number of chefs
    int dos_bonus = 0;              // the bonuses of a Dos connection: 0 off, 1 with the blue Poogie outfit, 2 the pink one
    bool instant_supply = false;    // items taken from the supply box without the delay
    int fov = 95;                   // field of view, 0..255 (95 is the game's)
    int camera = 3;                 // starting vertical camera position in quests: 0 lowest .. 3 default .. 4 highest
    bool offline_treasure = false;  // treasure quests solo in the offline Gathering Hall
};

inline constexpr std::size_t kConfigSize = 0x30;

// Writes `options` into CONFIG.BIN's bytes (the minimap at 100 %). False when `config` is too short to be one.
bool apply(std::vector<std::uint8_t> &config, const Options &options);

// The folder Freedom DX keeps its files in on the memory stick `stick`.
std::filesystem::path save_folder(const std::filesystem::path &stick);

// Writes `options` into <stick>/PSP/SAVEDATA/FDXDAT/CONFIG.BIN, making the folder and the file when they are missing.
// Returns an empty string, or what went wrong in words for the player.
std::string prepare(const std::filesystem::path &stick, const Options &options);

} // namespace mhf::launcher::fdx
