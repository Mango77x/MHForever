#pragma once

// Connects a PSP runtime to the desktop host: shows the displayed framebuffer, feeds controller input and plays the
// mixed audio. Used by the game executables and the homebrew runner when a window is wanted.

#include "mhf/psp/runtime.hpp"
#include "overlay/song_panel.hpp"

// host/keymap.hpp and host/padmap.hpp are not included here on purpose: the PSP runtime and the homebrew runners include this
// header without the SDL host's include directories. The keyboard and controller bindings travel as templates instead.
#include <functional>
#include <cstdint>
#include <memory>
#include <array>
#include <utility>
#include <vector>
#include <optional>
#include <string>

namespace mhf::psp {

class Frontend {
public:
    Frontend();
    ~Frontend();

    // Opens the window (hidden = no visible window, for tests). Returns false when no display is available.
    // With `gpu`, the GE draws through OpenGL at `render_scale` times the PSP resolution (software rendering is the
    // fallback when OpenGL 3.3 is missing). `fullscreen` starts in borderless fullscreen; `smooth_textures` is the
    // bilinear texture enhancement and `target_fps` the output rate: multiples of the game's 30 fps up to 180, made with in-between pictures.
    bool open(const char *title, bool hidden = false, int scale = 2, bool gpu = true, int render_scale = 4,
              bool fullscreen = false, bool smooth_textures = false, int target_fps = 30);

    // The same, with the keyboard and controller bindings the game was given (host::KeyBindings, host::PadBindings). A
    // template, so this header never has to know those types; the frontend instantiates it for the game.
    template <typename KeyBindings, typename PadBindings>
    bool open(const char *title, bool hidden, int scale, bool gpu, int render_scale, bool fullscreen,
              bool smooth_textures, int target_fps, const KeyBindings &keys, const PadBindings &pad) {
        const bool shown = open(title, hidden, scale, gpu, render_scale, fullscreen, smooth_textures, target_fps);
        set_bindings(keys, pad);
        return shown;
    }

    // Where the bindings changed in the in-game menu are written ("keymap=...\npadmap=...\n"), for the launcher to keep.
    // Empty (the default): changes last for the session.
    void set_controls_file(const std::string &path);
    // Friends in the Online tab (docs/14-rooms-and-friends.md section 4.3): the launcher keeps `friends` current while the
    // game runs; what the player does with them (invite, dismiss an invite) goes to `actions`.
    void set_friends_files(const std::string &friends, const std::string &actions);
    // A texture pack (frontend/texture_pack.hpp): its folder, and the release being played ("ULES01213"), which
    // chooses the pack's ini; `mipmaps` makes mip levels for its images that ship none, and `anisotropy` (1: off, 2, 4,
    // 8 or 16) filters them (gl_backend.hpp, LiveSettings). Before attach(). The Video tab of the in-game menu then
    // turns the pack on and off.
    void set_texture_pack(const std::string &folder, const std::string &game_id, bool mipmaps = false, int anisotropy = 16);
    // What a game knows about its textures, for the projected shadows (LiveSettings). Before attach().
    void set_shadow_textures(std::vector<std::uint64_t> air, std::vector<std::uint64_t> solid, std::vector<std::uint64_t> cave);
    void set_shadow_cave_zones(std::vector<std::string> zones);
    void set_shadow_haze_textures(std::vector<std::uint64_t> textures); // the shadows are worked out before these
    void set_shadow_zone_suns(std::vector<std::pair<std::string, std::array<float, 3>>> suns); // zone: height in degrees, darkness (< 0: 0.4), turn in degrees
    void set_shadow_zones(std::vector<std::string> names, std::vector<std::pair<std::uint32_t, int>> textures);
    // The quest HUD's size in percent, 50..100 (gl_backend.hpp, LiveSettings). The Video tab of the in-game menu
    // changes it while playing.
    void set_hud(int size);
    // The projected shadows as the launcher keeps them (gl_backend.hpp, LiveSettings): on, whole map, quality 0..2,
    // darkness 0..100. The Video tab changes them while playing, and the controls file keeps them for the launcher.
    void set_shadows(bool on, bool whole_map, int quality, int darkness);
    // Turns the hunting horn's songs on (an enhancement, off unless called) at the place the player chose; the Game tab
    // of the in-game menu changes the place or lets the player drag them anywhere, and the controls file keeps the new
    // spot for the launcher.
    void set_song_spot(overlay::SongSpot spot);
    // The songs can be shown at all (the game maps them): the Game tab then turns them on and off, and the controls file
    // keeps the choice (`horn-songs`). Without a call, set_song_spot turns them on with no switch.
    void offer_songs(bool on, overlay::SongSpot spot);

    // An option of the game the Game tab changes while playing (an enhancement whose hook reads it every time). The
    // controls file keeps its value for the launcher as "option.<setting>=true|false" (a switch) or "=<number>" (a
    // slider), where <setting> is the launcher's name for it.
    struct GameOption {
        std::string setting; // "fastSupplyBox"
        std::string label, detail;
        bool slider = false; // a switch (value 0 or 1) or a slider (min..max by step, in percent)
        int value = 0, min = 0, max = 1, step = 1;
        std::string shown_with; // the setting of a switch that must be on for this row to show ("analogCamera")
        std::function<void(int)> changed; // runs on the game's thread when the player changes it
    };
    void add_game_option(GameOption option);
    // The hunting horn's songs (overlay/song_panel.hpp), from the `extra` poll: shown at their place while the game draws
    // its quest HUD and no menu, system dialog or notice is up; std::nullopt (no horn, not in a quest) hides them. Only
    // the OpenGL renderer finds the quest HUD, so the software renderer never shows them.
    void set_song_panel(std::optional<overlay::SongPanel> panel);
    // Mouse look: while `wanted` says so and the game has the input (no menu, system dialog or song placing), the mouse
    // is held in the window and its motion reaches the game's hooks (ControllerState::mouse_x and mouse_y). Asked on the
    // game's thread at every poll.
    void set_mouse_look(std::function<bool()> wanted);

    // Installs the runtime's host hook. `extra` runs on every poll too (frame dumps, time limits...).
    void attach(Runtime &rt, std::function<void()> extra = {});

    struct Impl;

private:
    // Which physical key and which controller button stand for which pad button; defined in the frontend, which does know
    // host::KeyBindings and host::PadBindings.
    template <typename KeyBindings, typename PadBindings>
    void set_bindings(const KeyBindings &keys, const PadBindings &pad);

    std::unique_ptr<Impl> impl_;
};

} // namespace mhf::psp
