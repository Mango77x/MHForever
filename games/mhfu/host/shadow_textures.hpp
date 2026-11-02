#pragma once

// What the projected shadows should know about this game's textures (identification hashes, as the backend's texture cache
// computes them; "Test: identify centre" in the Video tab writes them to the log). Each entry beats the guesses by shape.

#include "shadow_zones.inc"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Mist and distant panels: they neither cast nor shade.
inline constexpr std::uint64_t kShadowAirTextures[] = {
    0xe9429f44c10c12a9ull, // the swamp's panels in the sky
    0xfdfd6dd715f1d518ull, // more of them, in its zone 2
    0x79aa69c00d47113dull, // the jungle's shapes in the sky
};

// Rock the guesses could take for mist.
inline constexpr std::uint64_t kShadowSolidTextures[] = {
    0x1bee8c61ccccad4aull,
    0x89ffa88c799638dcull, // desert mountains, taken for shells around the camera
    0xe3200ab39bfeba39ull,
    0x42cf792f5fb64e24ull,
    0x2027e778c124e9e7ull, // taken for mist in some pieces and not in others
    0x5f0482228ab53b4bull,
    0x33ed119b128bc331ull,
    0x00b84265ed5eed1aull,
    0x3510dbf03d994cf9ull, // snowy mountains, taken for a shell around the camera
    0x87ddd6c234261f5aull, // desert rock: its low tops must cast, unlike sand
    0xd7a3de44dfb2d944ull, // what shows through the gaps of a wall in the swamp, zone 4
    0x19b2be016a0a9a43ull, // and the wall
    0xf0d3f04a7cbcf2b3ull,
    0xeebb43ec35130882ull,
    0xaff7ca0369e01151ull, // thin trees in the swamp: low pieces must cast, unlike strips of ground
    0x678eabd8c2de845full, // lily pads: they lie flat and still cast
};

// The zones that are caves.
inline constexpr const char *kShadowCaveZones[] = {
    "areas/hunting/desert/base",
    "areas/hunting/desert/area3",         "areas/hunting/desert/area8",
    "areas/hunting/desert/area4",         "areas/hunting/desert/area6",         "areas/hunting/desert/area10",
    "areas/hunting/jungle/area6",         "areas/hunting/jungle/area7",         "areas/hunting/jungle/area8",
    "areas/hunting/snowy_mountains/area3", "areas/hunting/snowy_mountains/area4", "areas/hunting/snowy_mountains/area5",
    "areas/hunting/swamp/area3",          "areas/hunting/swamp/area7",          "areas/hunting/swamp/area9",
    "areas/hunting/volcano/area3",        "areas/hunting/volcano/area4",        "areas/hunting/volcano/area5",
    "areas/hunting/volcano/area6",        "areas/hunting/great_forest/area7",   "areas/hunting/great_forest/area8",
    "areas/hunting/forestandhills/base",  "areas/hunting/forestandhills/area5",
    // The village's buildings: everything there but its open air.
    "areas/pokke/farm",                   "areas/pokke/hall",                   "areas/pokke/house",
    "areas/pokke/kitchen",
};

inline std::vector<std::pair<std::uint32_t, int>> shadow_zone_textures() {
    std::vector<std::pair<std::uint32_t, int>> out;
    for (const ShadowZoneTexture &t : kShadowZoneTextures) out.emplace_back(t.hash, t.zone);
    return out;
}
