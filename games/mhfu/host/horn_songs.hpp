#pragma once

// The hunting horn's songs (docs/06-mhfu-module.md section 6): which songs the hunter's horn can play, read from the
// game's own tables, and how far the notes just played are into each of them. Freedom Unite only teaches the songs in
// two books in the hunter's house; the frontend shows them over the quest HUD instead (Frontend::set_song_panel).

#include "mhf/psp/runtime.hpp"
#include "overlay/song_panel.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace mhf::mhfu {

// A song as the game stores it: up to four note colours (1..7), the newest note first, 0 past its first note. A byte
// with 0x80 set asks for a note that is not that colour, so that a song of repeated notes (white, white) is not played
// again by one more of them.
using SongNotes = std::array<std::uint8_t, 4>;
// The notes played, the newest first (colours 1..7, 0 none yet), as the game keeps them on the player.
using PlayedNotes = std::array<std::uint8_t, 8>;

struct SongProgress {
    int played = 0;     // how many of the song's first notes are the last notes played (fewer than all of them)
    bool ready = false; // the game counts the song as played: it is ready to be performed
};

// The song's notes in playing order (oldest first), without the 0x80 bytes.
[[nodiscard]] std::vector<std::uint8_t> song_notes_in_order(const SongNotes &song);
// How far `played` is into `song`, by the rule the game applies after every note.
[[nodiscard]] SongProgress song_progress(const SongNotes &song, const PlayedNotes &played);

// The panel for the hunter on this machine, read on every host poll.
class HornSongs {
public:
    // std::nullopt outside quests, without a horn, or when this release does not map the addresses
    // (games/mhfu/regions/<disc>/symbols.toml).
    [[nodiscard]] std::optional<overlay::SongPanel> read(psp::Runtime &rt);

private:
    // The notes played stop counting a while after the last one: a song started long ago is not shown as under way.
    std::uint32_t player_ = 0;
    std::uint8_t note_count_ = 0;
    std::uint64_t note_at_us_ = 0; // 0: no note played in this quest yet
};

} // namespace mhf::mhfu
