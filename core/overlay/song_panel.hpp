#pragma once

// The hunting horn's songs over the quest HUD (MenuRenderer::render_song_panel): one row per song the horn can play, each
// with its notes in playing order. Console- and game-independent: the game's host fills it from the
// game's own tables and the notes the hunter has just played; the frontend shows it while the quest HUD is on screen.

#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mhf::overlay {

// Where the player wants the songs, out of the game HUD's way: under the health bar next to the horn's note sheet (the
// default), under the health bar in the middle, under the hunters' name plates, in the empty bottom left corner, or a
// spot of their own (dragged there in the game).
enum class SongPlace { next_to_sheet, top_middle, under_names, bottom_left, custom };
inline constexpr std::array<std::string_view, 4> kSongPlaceNames = {"next-to-sheet", "top-middle", "under-names", "bottom-left"};
inline constexpr std::array<std::string_view, 4> kSongPlaceLabels = {"Next to the sheet", "Top middle", "Under the names", "Bottom left"};

struct SongSpot {
    SongPlace place = SongPlace::next_to_sheet;
    int x = 0, y = 0; // custom: the card's top left corner, in units of the 480x272 canvas
    bool operator==(const SongSpot &) const = default;
};

// As the launcher and the controls file write it: a place's name, or "custom:X,Y".
inline std::string format_song_spot(const SongSpot &spot) {
    if (spot.place != SongPlace::custom) return std::string(kSongPlaceNames[static_cast<std::size_t>(spot.place)]);
    return "custom:" + std::to_string(spot.x) + "," + std::to_string(spot.y);
}
inline std::optional<SongSpot> parse_song_spot(std::string_view text) {
    for (std::size_t i = 0; i < kSongPlaceNames.size(); ++i) {
        if (kSongPlaceNames[i] == text) return SongSpot{static_cast<SongPlace>(i)};
    }
    constexpr std::string_view kCustom = "custom:";
    if (!text.starts_with(kCustom)) return std::nullopt;
    text.remove_prefix(kCustom.size());
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos) return std::nullopt;
    SongSpot spot{SongPlace::custom};
    const auto read = [](std::string_view s, int &v, int hi) {
        const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), v);
        return error == std::errc{} && end == s.data() + s.size() && v >= 0 && v <= hi;
    };
    if (!read(text.substr(0, comma), spot.x, 480) || !read(text.substr(comma + 1), spot.y, 272)) return std::nullopt;
    return spot;
}

struct SongPanel {
    struct Note {
        float r = 1.0f, g = 1.0f, b = 1.0f;
        bool operator==(const Note &) const = default;
    };
    struct Song {
        std::string name;
        std::vector<Note> notes; // in playing order
        int played = 0;          // how many of its first notes are the last notes played (0: not started)
        bool ready = false;      // all of them, as the game counts it: the song is ready to be performed
        bool active = false;     // its effect is on the hunter now
        bool operator==(const Song &) const = default;
    };
    std::vector<Song> songs;
    bool operator==(const SongPanel &) const = default;
};

} // namespace mhf::overlay
