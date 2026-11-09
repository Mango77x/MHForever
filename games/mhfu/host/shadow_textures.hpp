#pragma once

// What the projected shadows should know about this game's textures (identification hashes, as the backend's texture cache
// computes them; "Test: identify centre" in the Video tab writes them to the log). Each entry beats the guesses by shape.

#include "shadow_zones.inc"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Mist and distant panels: they neither cast nor shade.
inline constexpr std::uint64_t kShadowAirTextures[] = {
    0x79aa69c00d47113dull, // the jungle's shapes in the sky
    0xb40509f9bb505fcbull, // the sky over its cave, zone 7
};

// Haze the game draws right in front of the camera, writing depth: the shadows are worked out before it.
inline constexpr std::uint64_t kShadowHazeTextures[] = {
    0xfab96254c5ff1085ull, // forest and hills
    0x75ec9e9692beacf5ull, // and in its zone 10
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
    "areas/hunting/desert/area10",
    "areas/hunting/snowy_mountains/area3", "areas/hunting/snowy_mountains/area4", "areas/hunting/snowy_mountains/area5",
    "areas/hunting/swamp/area3",          "areas/hunting/swamp/area9",
    "areas/hunting/volcano/area5",        "areas/hunting/great_forest/area7",   "areas/hunting/great_forest/area8",
    "areas/hunting/old_jungle/area4",     "areas/hunting/old_jungle/area9",     "areas/hunting/old_jungle/area10",
    // The village's buildings: everything there but its open air.
    "areas/pokke/farm",                   "areas/pokke/hall",                   "areas/pokke/house",
    "areas/pokke/kitchen",
    "areas/hunting/fortress/base",        "areas/hunting/fortress/area2",       "areas/hunting/fortress/area3",
    "areas/hunting/fortress/area4",       "areas/hunting/town/area3",           "areas/hunting/castle_schrade/area1",
    "areas/hunting/snowy_mountains_peak/area1",
    "areas/hunting/old_swamp/area7",      "areas/hunting/old_swamp/area8",      "areas/hunting/old_swamp/area11",
};

// How high the sun stands in some zones, in degrees over the horizon (below 0: where the game has it), and
// how dark the shadows are there, as on the "Shadow darkness" slider (nothing: as that slider says), and how far the sun is
// turned from where the game has it, as on the "Sun direction" slider (0..100: a whole turn; nothing: not at all).
struct ShadowZoneSun {
    const char *zone;
    float height;
    int darkness = -1;
    float turn = 0;
};
inline constexpr ShadowZoneSun kShadowZoneSuns[] = {
    {"areas/hunting/forestandhills/area8", 43.25f},  {"areas/hunting/forestandhills/area6", 44.5f, -1, 95},
    {"areas/hunting/forestandhills/area7", 68.0f},  {"areas/hunting/forestandhills/area10", 68.0f},
    {"areas/hunting/forestandhills/area11", 68.0f}, {"areas/hunting/forestandhills/area12", 68.0f},
    {"areas/hunting/forestandhills/area9", 90.0f},
    {"areas/hunting/desert/base", 44.5f, -1, 65},       {"areas/hunting/desert/area4", 84.5f, 30},
    {"areas/hunting/desert/area6", 35.5f, 40, 60},      {"areas/hunting/desert/area3", 21.8f, 20},
    {"areas/hunting/desert/area8", 70.75f},          {"areas/hunting/desert/area7", -1.0f, 20, 40},
    {"areas/hunting/desert/area9", 62.7f, -1, 25},         {"areas/hunting/desert/area5", 44.5f, 35, 95},
    {"areas/hunting/jungle/area6", 90.0f, 30},     {"areas/hunting/jungle/area7", -1.0f, -1, 80},
    {"areas/hunting/jungle/area8", -1.0f, 40, 90},
    {"areas/hunting/jungle/area3", -1.0f, 50},
    {"areas/hunting/jungle/area5", -1.0f, 50},
    {"areas/hunting/snowy_mountains/area5", 40.0f, 30, 45},
    {"areas/hunting/snowy_mountains/area6", 40.0f, 30, 50},
    {"areas/hunting/snowy_mountains/area3", 71.8f},
    {"areas/hunting/snowy_mountains/area2", 35.5f, -1, 50},
    {"areas/hunting/volcano/base", -1.0f, 55},
    {"areas/hunting/volcano/area1", -1.0f, 55},
    {"areas/hunting/volcano/area2", -1.0f, 55},
    {"areas/hunting/volcano/area3", 12.7f, 40, 75},
    {"areas/hunting/volcano/area4", 17.3f, 40, 90},
    {"areas/hunting/volcano/area5", 30.9f, 55, 30},
    {"areas/hunting/volcano/area6", 30.9f, 55, 75},
    {"areas/hunting/volcano/area7", -1.0f, 55},
    {"areas/hunting/volcano/area8", -1.0f, 65},
    {"areas/hunting/volcano/area9", -1.0f, 55},
    {"areas/hunting/volcano/area10", -1.0f, 55},
    {"areas/hunting/tower_1_2/area7_9", 17.3f, 30, 100},
    {"areas/hunting/tower_1_2/area4", 35.5f, -1, 25},
    {"areas/hunting/tower_1_2/area5", 35.5f, -1, 25},
    {"areas/hunting/tower_1_2/area3", 90.0f},
    {"areas/hunting/tower_1_2/area6_8", 26.4f, -1, 45},
    {"areas/hunting/great_forest/base", -1.0f, 50},
    {"areas/hunting/great_forest/area1", -1.0f, 50},
    {"areas/hunting/great_forest/area2", -1.0f, 50},
    {"areas/hunting/great_forest/area3", -1.0f, 50, 90},
    {"areas/hunting/great_forest/area4", -1.0f, 50},
    {"areas/hunting/great_forest/area5", -1.0f, 50},
    {"areas/hunting/great_forest/area6", -1.0f, 40},
    {"areas/hunting/great_forest/area7", -1.0f, 50},
    {"areas/hunting/great_forest/area8", 90.0f, 50},
    {"areas/hunting/old_jungle/area6", -1.0f, -1, 15},
    {"areas/hunting/old_jungle/area7", -1.0f, -1, 55},
    {"areas/hunting/old_jungle/area4", 90.0f},
    {"areas/hunting/old_jungle/area8", 90.0f},
    {"areas/hunting/old_jungle/area9", 90.0f},
    {"areas/hunting/old_jungle/area10", 90.0f},
    {"areas/hunting/arena/base", 40.0f, -1, 45},
    {"areas/hunting/arena/area1", 30.9f},
    {"areas/pokke/hall", 53.6f, 30, 85},
    // The fortress.
    {"areas/hunting/fortress/base", 90.0f},
    {"areas/hunting/fortress/area1", -1.0f, -1, 55},
    {"areas/hunting/fortress/area6", 40.0f, -1, 85},
    {"areas/hunting/castle_schrade/area1", 90.0f},
    {"areas/hunting/snowy_mountains_peak/area1", -1.0f, 20},
    {"areas/hunting/old_swamp/area7", 90.0f},
    {"areas/hunting/old_swamp/area8", 90.0f},
    {"areas/hunting/old_swamp/area11", 90.0f},
    {"areas/hunting/old_swamp/area5", 44.5f},
    {"areas/hunting/old_swamp/area10", 44.5f},
    {"areas/hunting/swamp/area7", 12.7f},
    {"areas/hunting/swamp/area4", -1.0f, 50},
    {"areas/hunting/swamp/area5", -1.0f, 50},
    {"areas/hunting/swamp/area6", -1.0f, 50},
};

// The same for the night, when the game's light stands somewhere else, and the turn counts from there.
inline constexpr ShadowZoneSun kShadowZoneNightSuns[] = {
    {"areas/hunting/desert/base", -1.0f, 20},
    {"areas/hunting/desert/area1", -1.0f, 20, 90},
    {"areas/hunting/desert/area2", -1.0f, 20, 10},
    {"areas/hunting/desert/area3", 30.9f, 20, 5},
    {"areas/hunting/desert/area4", 84.5f, 20, 75}, // the game's light: 135 by day, -135 at night
    {"areas/hunting/desert/area5", -1.0f, 20, 10},
    {"areas/hunting/desert/area6", 35.5f, 20, 60},
    {"areas/hunting/desert/area7", -1.0f, 20, 40},
    {"areas/hunting/desert/area8", -1.0f, 20},
    {"areas/hunting/desert/area9", -1.0f, 20},
    {"areas/hunting/desert/area10", -1.0f, 20, 25}, // -45 by day, -135 at night
    // The fortress, castle Schrade, the peak and the old swamp as by day, should their sky be taken for night.
    {"areas/hunting/fortress/base", 90.0f},
    {"areas/hunting/fortress/area1", -1.0f, -1, 55},
    {"areas/hunting/fortress/area6", 40.0f, -1, 85},
    {"areas/hunting/castle_schrade/area1", 90.0f},
    {"areas/hunting/snowy_mountains_peak/area1", -1.0f, 20},
    {"areas/hunting/old_swamp/area7", 90.0f},
    {"areas/hunting/old_swamp/area8", 90.0f},
    {"areas/hunting/old_swamp/area11", 90.0f},
    {"areas/hunting/old_swamp/area5", 44.5f},
    {"areas/hunting/old_swamp/area10", 44.5f},
    // The volcano as by day. The game's light is the same by night, but in the camp, where it stands lower. Darkness at 40 all over.
    {"areas/hunting/volcano/base", 73.3f, 40},
    {"areas/hunting/volcano/area1", -1.0f, 40},
    {"areas/hunting/volcano/area2", -1.0f, 40},
    {"areas/hunting/volcano/area3", 12.7f, 40, 75},
    {"areas/hunting/volcano/area4", 17.3f, 40, 90},
    {"areas/hunting/volcano/area5", 30.9f, 40, 30},
    {"areas/hunting/volcano/area6", 30.9f, 40, 75},
    {"areas/hunting/volcano/area7", -1.0f, 40},
    {"areas/hunting/volcano/area8", -1.0f, 40},
    {"areas/hunting/volcano/area9", -1.0f, 40},
    {"areas/hunting/volcano/area10", -1.0f, 40},
    // The snowy mountains: the moon as the game has it, with light shadows. Zones 3 and 5 keep the sun of the day.
    {"areas/hunting/snowy_mountains/base", -1.0f, 20},
    {"areas/hunting/snowy_mountains/area1", -1.0f, 20},
    {"areas/hunting/snowy_mountains/area2", -1.0f, 20},
    {"areas/hunting/snowy_mountains/area3", 71.8f, 20},
    {"areas/hunting/snowy_mountains/area4", -1.0f, 20},
    {"areas/hunting/snowy_mountains/area5", 40.0f, 20, 45},
    {"areas/hunting/snowy_mountains/area6", -1.0f, 20},
    {"areas/hunting/snowy_mountains/area7", -1.0f, 20},
    {"areas/hunting/snowy_mountains/area8", -1.0f, 20},
    // The swamp by day: the game's light moves in zone 8, from -90 to 45 degrees and lower.
    {"areas/hunting/swamp/area8", 45.0f, -1, 62.5f},
    // The jungle by day: zone 3's light moves at night, the others' does not. All of it at 30 darkness by night, but zones 3, 5 and 8 at 40.
    {"areas/hunting/jungle/area3", 35.3f, 40, 12.5f},
    {"areas/hunting/jungle/area6", 90.0f, 30},
    {"areas/hunting/jungle/area7", -1.0f, 30, 80},
    {"areas/hunting/jungle/area8", -1.0f, 40, 90},
    {"areas/hunting/jungle/base", -1.0f, 30},
    {"areas/hunting/jungle/area1", -1.0f, 30},
    {"areas/hunting/jungle/area2", -1.0f, 30},
    {"areas/hunting/jungle/area4", -1.0f, 30},
    {"areas/hunting/jungle/area5", -1.0f, 40},
    {"areas/hunting/jungle/area9", -1.0f, 30},
    {"areas/hunting/jungle/area10", -1.0f, 30},
};

inline std::vector<std::pair<std::string, std::array<float, 3>>> shadow_zone_suns() {
    std::vector<std::pair<std::string, std::array<float, 3>>> out;
    for (const ShadowZoneSun &s : kShadowZoneSuns) {
        out.emplace_back(s.zone, std::array<float, 3>{s.height, static_cast<float>(s.darkness) / 100.0f,
                                                      3.6f * s.turn});
    }
    for (const ShadowZoneSun &s : kShadowZoneNightSuns) {
        out.emplace_back(std::string("night:") + s.zone, std::array<float, 3>{s.height, static_cast<float>(s.darkness) / 100.0f,
                                                                              3.6f * s.turn});
    }
    return out;
}

inline std::vector<std::pair<std::uint32_t, int>> shadow_zone_textures() {
    std::vector<std::pair<std::uint32_t, int>> out;
    for (const ShadowZoneTexture &t : kShadowZoneTextures) out.emplace_back(t.hash, t.zone);
    return out;
}
