#include "horn_songs.hpp"

#include "mhfu_region_data.hpp" // generated from this release's regions/<disc>/symbols.toml [data]

#include <algorithm>
#include <string_view>

namespace mhf::mhfu {
namespace {

namespace region = psp::generated::mhfu_region_data;

// Struct offsets, the same in the three releases (each checked in their code: the same instructions read them).
constexpr std::uint32_t kGameSysPlayerId = 0x28;         // u8: the player slot this machine plays
constexpr std::uint32_t kGameSysWeapon = 0x4A0 + 0x20;   // the hunter's weapon: u8 type at +1 (5 blademaster), u16 id at +2
constexpr std::uint32_t kPlayerManagerPlayers = 0x0C;    // Player*[6], by player slot
constexpr std::uint32_t kPlayerNoteCount = 0x1102;       // u8, one more for every note played
constexpr std::uint32_t kPlayerNotesPlayed = 0x1103;     // u8[8], the newest first
// The songs' effects: a time left in frames per effect (the game's own "is it on" test is that it is above 0), and a
// level per effect, where the [Lo] songs write 2 and the [Hi] songs 3 (4 after an encore).
constexpr std::uint32_t kPlayerSongTimers = 0x6A0;       // u16[21]
constexpr std::uint32_t kPlayerSongLevels = 0x5F7;       // u8[21]
constexpr std::uint32_t kSongEffects = 21;
constexpr std::uint32_t kWeaponInfoSize = 24, kWeaponNoteSet = 0x13;
constexpr std::uint8_t kBlademasterWeapon = 5, kHuntingHorn = 8;
constexpr std::uint32_t kPlayerSlots = 6, kNoteSets = 20, kSongs = 47;
constexpr std::uint64_t kNotesStayUs = 6'000'000; // the notes played count this long after the last one
// The quest text (mh_resource_manager, mh_quest_message_texts): the resource manager keeps a table of the loaded
// resources, {u32 id, u32 data, u32} each; the quest text is resource 4, and the messages of the songs' effects are in
// its text group 1, whose start is at +12 in the resource: a u32 offset per text id (from the group's start, maybe not
// aligned, 0xFFFFFFFF for none), then the UTF-8 texts.
constexpr std::uint32_t kResourceTable = 0x83C030, kResourceEntry = 12, kResources = 512;
constexpr std::uint32_t kQuestText = 4, kSongTextGroup = 1;
constexpr std::uint32_t kLongestName = 64; // bytes; a longer text is not a song's name

constexpr bool kMapped = region::mh_game_sys && region::mh_player_manager && region::mh_blademaster_weapons &&
                         region::mh_weapon_model_classes && region::mh_horn_note_colors && region::mh_horn_songs;
constexpr bool kTextMapped = region::mh_resource_manager && region::mh_quest_message_texts;

// The message the game shows when each song is played, in its song order: its effect, as the quest text names it in the
// language the game runs in. The heals show the weaker or the stronger of two by chance: the stronger, which the books'
// names follow.
constexpr std::uint8_t kSongMessages[] = {
    0,  0,  2,  4,  5,  7,  7,  8,  9,  10, 11, 12, 12, 13, 14, 15, 17, 19, 20, 21, 23, 25, 26, 27,
    28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 40, 41, 43, 44, 46, 47, 47, 49, 49, 50, 52, 53,
};
static_assert(std::size(kSongMessages) == kSongs);

// The songs in the game's order (its song table, mh_horn_songs), as the Hunting Horn books of the English release
// name them: shown when the game's own text cannot be read.
constexpr std::string_view kSongNames[kSongs] = {
    "Self-Improvement", "Self-Improvement", "Attack Up [Lo]", "Attack Up [Hi]", "Defense Up [Lo]", "Defense Up [Hi]",
    "Defense Up [Hi]", "Health Inc [Lo]", "Health Inc [Mid]", "Health Inc [Hi]", "Wind Reduce", "Wind Cancel",
    "Wind Cancel", "Wind All Cancel", "Marathon [Lo]", "Marathon [Hi]", "Clairvoyance", "Health Rec [Lo]",
    "Health Rec [Mid]", "Health Rec [Hi]", "Health Rec [Mid] & Deodorant", "Health Rec [Lo] & Antidote",
    "Health Rec [Mid] & Antidote", "Recover Spd Up [Lo]", "Recover Spd Up [Hi]", "Heavenly Protection",
    "No Cold & Snow Res", "No Heat", "Hear Protect [Lo]", "Hear Protect [Hi]", "No Faint", "No Paralysis", "No Quakes",
    "Element Attack Up", "Fire Res [Lo]", "Fire Res [Hi]", "Water Res [Lo]", "Water Res [Hi]", "Thunder Res [Lo]",
    "Thunder Res [Hi]", "Ice Res [Lo]", "Ice Res [Lo]", "Ice Res [Hi]", "Ice Res [Hi]", "Dragon Res [Lo]",
    "Dragon Res [Hi]", "Supersonic Waves",
};

// Each song's effect, in the game's song order: its index in the player's effect timers, -1 for the songs that act
// once (the heals, Supersonic Waves); and for the [Lo]/[Hi] pairs whose level tells them apart, which one it is
// (1 [Lo], 2 [Hi], 0 the effect whatever its level).
struct SongEffect {
    std::int8_t timer, tier;
};
constexpr SongEffect kSongEffectsBySong[kSongs] = {
    {0, 0},  {0, 0},  {1, 1},  {1, 2},  {2, 1},  {2, 2},  {2, 2},  {3, 0},  {3, 0},  {3, 0},  {4, 0},  {4, 0},
    {4, 0},  {4, 0},  {5, 0},  {5, 0},  {6, 0},  {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {7, 0},
    {7, 0},  {8, 0},  {9, 0},  {10, 0}, {11, 0}, {11, 0}, {12, 0}, {13, 0}, {14, 0}, {15, 0}, {16, 1}, {16, 2},
    {17, 1}, {17, 2}, {18, 1}, {18, 2}, {19, 1}, {19, 1}, {19, 2}, {19, 2}, {20, 1}, {20, 2}, {-1, 0},
};

// The note colours as the game draws them: white, purple, blue, yellow, red, green, sky blue.
overlay::SongPanel::Note note_color(std::uint8_t note) {
    switch (note) {
    case 1: return {0.95f, 0.95f, 0.95f};
    case 2: return {0.86f, 0.33f, 0.95f};
    case 3: return {0.28f, 0.40f, 1.00f};
    case 4: return {1.00f, 0.90f, 0.18f};
    case 5: return {1.00f, 0.22f, 0.20f};
    case 6: return {0.25f, 0.88f, 0.30f};
    case 7: return {0.45f, 0.80f, 1.00f};
    default: return {0.5f, 0.5f, 0.5f};
    }
}

// True when `text` is well-formed UTF-8 without control characters: the game's text as the overlay's painter takes it.
bool printable_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        const std::size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + length > text.size() || (length == 1 && c < 0x20)) return false;
        for (std::size_t k = 1; k < length; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        }
        i += length;
    }
    return !text.empty();
}

// The quest text's message `message` in the language the game runs in, std::nullopt when it cannot be read.
std::optional<std::string> quest_message(const psprecomp::GuestMemory &memory, std::uint32_t message) {
    const std::uint32_t manager = memory.load32(*region::mh_resource_manager);
    if (!memory.contains(manager + kResourceTable, kResources * kResourceEntry)) return std::nullopt;
    std::uint32_t resource = 0;
    for (std::uint32_t i = 0; i < kResources && resource == 0; ++i) {
        if (memory.load32(manager + kResourceTable + i * kResourceEntry) == kQuestText) {
            resource = memory.load32(manager + kResourceTable + i * kResourceEntry + 4);
        }
    }
    if (!memory.contains(resource + (kSongTextGroup + 2) * 4, 4)) return std::nullopt;
    const std::uint32_t group = resource + memory.load32(resource + (kSongTextGroup + 2) * 4);
    const std::uint32_t id = memory.load16(*region::mh_quest_message_texts + message * 2);
    if (!memory.contains(group + id * 4, 4)) return std::nullopt;
    std::uint32_t offset = 0;
    for (std::uint32_t k = 0; k < 4; ++k) offset |= static_cast<std::uint32_t>(memory.load8(group + id * 4 + k)) << (8 * k);
    if (offset == 0xFFFFFFFFu || !memory.contains(group + offset, kLongestName)) return std::nullopt;
    std::string text;
    for (std::uint32_t k = 0; k < kLongestName; ++k) {
        const auto c = static_cast<char>(memory.load8(group + offset + k));
        if (c == '\0') return printable_utf8(text) ? std::optional(text) : std::nullopt;
        text.push_back(c);
    }
    return std::nullopt;
}

// The Player* of the hunter on this machine, 0 when there is none (not in a quest).
std::uint32_t local_player(const psprecomp::GuestMemory &memory, std::uint32_t game_sys) {
    const std::uint32_t slot = memory.load8(game_sys + kGameSysPlayerId);
    const std::uint32_t manager = memory.load32(*region::mh_player_manager);
    if (slot >= kPlayerSlots || !memory.contains(manager + kPlayerManagerPlayers, kPlayerSlots * 4)) return 0;
    const std::uint32_t player = memory.load32(manager + kPlayerManagerPlayers + slot * 4);
    return memory.contains(player + kPlayerNotesPlayed, 8) ? player : 0;
}

} // namespace

std::vector<std::uint8_t> song_notes_in_order(const SongNotes &song) {
    std::vector<std::uint8_t> notes;
    for (const std::uint8_t b : song) {
        if (b != 0 && (b & 0x80u) == 0) notes.insert(notes.begin(), b);
    }
    return notes;
}

SongProgress song_progress(const SongNotes &song, const PlayedNotes &played) {
    SongProgress progress;
    // The game's rule: the song is played when each of its four bytes holds for the note as old: 0 for any note, a
    // colour for that colour, 0x80 | a colour for any other.
    progress.ready = std::ranges::all_of(std::array{0, 1, 2, 3}, [&](int i) {
        const std::uint8_t want = song[static_cast<std::size_t>(i)], note = played[static_cast<std::size_t>(i)];
        if (want == 0) return true;
        return (want & 0x80u) != 0 ? note != (want & 0x7Fu) : note == want;
    });
    // Under way: the last k notes played are its first k notes.
    const std::vector<std::uint8_t> notes = song_notes_in_order(song);
    for (std::size_t k = notes.empty() ? 0 : notes.size() - 1; k > 0; --k) {
        bool under_way = true;
        for (std::size_t i = 0; i < k && under_way; ++i) under_way = played[i] == notes[k - 1 - i];
        if (under_way) {
            progress.played = static_cast<int>(k);
            break;
        }
    }
    return progress;
}

std::optional<overlay::SongPanel> HornSongs::read(psp::Runtime &rt) {
    if constexpr (!kMapped) {
        (void)rt;
        return std::nullopt;
    } else {
        // The song table lives in the quest overlay (game_task.ovl); anywhere else that memory holds other code.
        const psp::Program *overlay = rt.active_overlay_at(*region::mh_horn_songs);
        if (overlay == nullptr || !std::string_view(overlay->name).ends_with("_ovl_game_task")) return std::nullopt;
        const psprecomp::GuestMemory &memory = rt.memory();
        const std::uint32_t game_sys = memory.load32(*region::mh_game_sys);
        if (!memory.contains(game_sys + kGameSysWeapon, 4)) return std::nullopt;
        if (memory.load8(game_sys + kGameSysWeapon + 1) != kBlademasterWeapon) return std::nullopt;
        const std::uint32_t weapon = *region::mh_blademaster_weapons + memory.load16(game_sys + kGameSysWeapon + 2) * kWeaponInfoSize;
        if (memory.load8(*region::mh_weapon_model_classes + memory.load16(weapon)) != kHuntingHorn) return std::nullopt;
        const std::uint32_t note_set = memory.load8(weapon + kWeaponNoteSet);
        if (note_set >= kNoteSets) return std::nullopt;

        std::array<std::uint8_t, 3> horn{};
        overlay::SongPanel panel;
        for (std::uint32_t i = 0; i < 3; ++i) {
            horn[i] = memory.load8(*region::mh_horn_note_colors + note_set * 3 + i);
        }
        PlayedNotes played{};
        const std::uint32_t player = local_player(memory, game_sys);
        const bool effects = player != 0 && memory.contains(player + kPlayerSongTimers, kSongEffects * 2) &&
                             memory.contains(player + kPlayerSongLevels, kSongEffects);
        if (player != 0) {
            const std::uint8_t count = memory.load8(player + kPlayerNoteCount);
            const std::uint64_t now = rt.now_us();
            if (player != player_) { // a new quest: the notes it starts with are old
                player_ = player;
                note_count_ = count;
                note_at_us_ = 0;
            } else if (count != note_count_) {
                note_count_ = count;
                note_at_us_ = now;
            }
            if (note_at_us_ != 0 && now - note_at_us_ < kNotesStayUs) {
                for (std::uint32_t i = 0; i < played.size(); ++i) played[i] = memory.load8(player + kPlayerNotesPlayed + i);
            }
        }
        for (std::uint32_t s = 0; s < kSongs; ++s) {
            SongNotes song{};
            for (std::uint32_t i = 0; i < 4; ++i) song[i] = memory.load8(*region::mh_horn_songs + s * 4 + i);
            const std::vector<std::uint8_t> notes = song_notes_in_order(song);
            // A horn plays the songs made of its own notes only.
            if (notes.empty() || !std::ranges::all_of(notes, [&](std::uint8_t n) { return std::ranges::find(horn, n) != horn.end(); })) continue;
            overlay::SongPanel::Song row;
            std::optional<std::string> name;
            if constexpr (kTextMapped) name = quest_message(memory, kSongMessages[s]);
            row.name = name ? std::move(*name) : std::string(kSongNames[s]);
            for (const std::uint8_t n : notes) row.notes.push_back(note_color(n));
            const SongProgress progress = song_progress(song, played);
            row.played = progress.played;
            row.ready = progress.ready;
            if (const SongEffect effect = kSongEffectsBySong[s]; effects && effect.timer >= 0) {
                const auto slot = static_cast<std::uint32_t>(effect.timer);
                const std::uint8_t level = memory.load8(player + kPlayerSongLevels + slot);
                row.active = memory.load16(player + kPlayerSongTimers + slot * 2) > 0 &&
                             (effect.tier == 0 || (effect.tier == 2) == (level >= 3));
            }
            panel.songs.push_back(std::move(row));
        }
        return panel;
    }
}

} // namespace mhf::mhfu
