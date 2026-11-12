#pragma once

// OpenGL 3.3 drawing for the GE (docs/05-runtime-hle.md §6, "hardware renderer").
//
// Runs on the GE thread: it owns the OpenGL context of the game window, draws every framebuffer the game renders
// into an upscaled render target, and presents the displayed one to the window.

#include "backend.hpp"

#include "psprecomp/guest_memory.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <array>
#include <utility>
#include <vector>

struct SDL_Window;

namespace mhf::psp {

// An image drawn over the game (system dialogs), handed from the emulation thread to the GE thread. Premultiplied
// RGBA covering the letterboxed game area; `generation` changes whenever the image does.
struct OverlayImage {
    std::mutex mutex;
    std::vector<std::uint32_t> pixels;
    int width = 0, height = 0;
    bool visible = false;
    // The image is the in-game menu: the game runs on behind it, blurred, and the backend draws it at least as often as the
    // display refreshes, also when the game shows nothing new.
    bool continuous = false;
    // The shape it covers: 0 the PSP's 480:272, centred in the window; wider, the stretched widescreen picture (the
    // hunting horn's songs, which sit by the quest HUD).
    float aspect = 0.0f;
    std::uint64_t generation = 0;
};

// What the player can change while the game runs (read by the GE thread).
struct LiveSettings {
    std::atomic<bool> smooth_textures{false};
    // Requests for the drawing thread, taken at the start of the next picture (0 = none): the internal resolution
    // multiplier (1..10) and the output rate in fps (a multiple of 30 up to 180).
    std::atomic<int> scale{0};
    std::atomic<int> fps{0};
    // Texture pack (frontend/texture_pack.hpp): drawn or not while the game runs; and, set before the backend is made,
    // the pack's folder (empty: none, or MHF_TEXTURE_PACK) and the release it is drawn for ("ULES01213").
    std::atomic<bool> texture_pack{true};
    std::string texture_pack_folder, texture_pack_game;
    // The textures MHForever ships (a pack in PPSSPP's format, fonts/hd-text: the HD text by Kaneda), drawn with or
    // without the player's pack; the player's pack wins for a texture both have. Empty: none.
    std::string builtin_textures_folder;
    // Its filtering, changed in the menu while the game runs: mip levels made for images that ship none (softer far
    // away, no shimmer; off draws them as PPSSPP does), and the anisotropic filtering of its images, 1 (off) to 16.
    std::atomic<bool> texture_pack_mipmaps{false};
    std::atomic<int> texture_pack_anisotropy{16};
    // Projected shadows (enhancement): on or off while the game runs (MHF_SHADOW=1 starts with them on). Skinned draws (the
    // hunter, the monsters) always cast. shadow_whole_map: all the solid scenery casts too, and surfaces turned away from the sun
    // are shaded; off, only the hunter and monsters cast. shadow_quality: the size of the map of shadows, 0 low (2048), 1 medium
    // (4096), 2 high (8192); low unless chosen. shadow_strength: how dark, 0..1; at 0.5 each zone is as it was tuned, below it lighter down to no
    // shade at 0, above it darker up to half as dark again at 1.
    std::atomic<bool> shadow{false};
    std::atomic<bool> shadow_whole_map{true};
    std::atomic<int> shadow_quality{0};
    // What the game knows about its own textures (Frontend::set_shadow_textures), set before the backend is made: air (mist,
    // haze), solid (rock that looks like mist to the guesses), and the insides of caves, where only objects cast.
    std::vector<std::uint64_t> shadow_air_textures, shadow_solid_textures, shadow_cave_textures, shadow_haze_textures;
    // Where the game is: the zones of its maps ("areas/hunting/desert/area6"), the game's textures that only a zone uses (a hash
    // of the game's own texture, as packs key them, and an index into the names), and the zones that are caves.
    std::vector<std::string> shadow_zone_names, shadow_cave_zones;
    std::vector<std::pair<std::string, std::array<float, 3>>> shadow_zone_suns; // some zones' sun: height in degrees, darkness (< 0: 0.4), turn in degrees
    std::vector<std::pair<std::uint32_t, int>> shadow_zone_textures;
    std::atomic<float> shadow_strength{0.5f};
    // What casts, set by the backend as it goes in and out of caves: objects up to shadow_max_object long (world units), and
    // inside the scenery slender pieces up to shadow_max_piece (0: none).
    std::atomic<float> shadow_max_object{39450.0f};
    std::atomic<float> shadow_max_piece{39450.0f};
    // HUD size in percent (100: as the game draws it): in quests, the HUD shrinks into the corners it sits in.
    std::atomic<int> hud_size{100};
    // Widescreen (enhancement, off by default): the game's 3D is drawn at the window's shape and the picture stretched
    // to it, 2D kept in shape (gl_backend.cpp, size_hud). wide_aspect is that shape (width over height), worked out by
    // the frontend at every poll: 0 while off, with the software renderer or in a window no wider than 480:272.
    std::atomic<bool> widescreen{false};
    std::atomic<float> wide_aspect{0.0f};
    // Written by the drawing thread at every flip: that flip drew the quest HUD (its clock). The hunting horn's song
    // panel is only shown over it.
    std::atomic<bool> quest_hud{false};
};

// Creates the context on the calling thread. Returns nullptr (after logging why) when OpenGL 3.3 is unavailable.
// `scale` is the internal resolution multiplier (1 = the original 480x272). `smooth_textures` (enhancement) forces
// bilinear filtering on 3D textures. `target_fps` (enhancement) is the output rate: a whole multiple of the game's 30 fps up to 180 (rounded), with
// in-between pictures drawn for every flip; 30 shows the game as it is.
// `live` (optional) is read every draw and starts with the caller's value; without it `smooth_textures` is fixed.
std::unique_ptr<gpu::Backend> make_gl_backend(SDL_Window *window, int scale, psprecomp::GuestMemory &memory,
                                              bool smooth_textures = false, OverlayImage *overlay = nullptr,
                                              int target_fps = 30, LiveSettings *live = nullptr);

} // namespace mhf::psp
