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
    // Projected shadows (enhancement): on or off while the game runs and how dark they are (0..1). Skinned draws (the hunter, the
    // monsters) always cast; anything else casts if the object is at most shadow_max_object long (world units), which leaves
    // the scenery out. Inside the scenery, slender solid pieces up to shadow_max_piece cast too (trunks, posts; 0: none),
    // and so do leaves and fronds. shadow_show paints what casts over the picture. MHF_SHADOW=1, MHF_SHADOW_STRENGTH,
    // MHF_SHADOW_MAX_OBJECT, MHF_SHADOW_MAX_PIECE and MHF_SHADOW_SHOW=1 set the values the game starts with.
    // shadow_whole_map (MHF_SHADOW_WHOLE_MAP=0 starts with it off): all the solid scenery casts too, and surfaces turned away
    // from the sun are shaded; the size limits below then apply only to leaves.
    std::atomic<bool> shadow{false};
    std::atomic<bool> shadow_whole_map{true};
    std::atomic<bool> shadow_whole_map_in_caves{false}; // the zones listed as caves get the whole map too
    std::atomic<float> shadow_sun_height{0.0f};         // degrees the sun is raised to at least; 0: as the game has it
    // For finding faults: 0 everything, 1 only the shadows that fall on things, 2 only the shade of sides turned from the sun.
    std::atomic<int> shadow_part{0};
    // For identifying things: what is under the middle of the screen goes to the log, with its texture.
    std::atomic<bool> shadow_pick{false};
    // What the game knows about its own textures (Frontend::set_shadow_textures), set before the backend is made: air (mist,
    // haze), solid (rock that looks like mist to the guesses), and the insides of caves, where only objects cast.
    std::vector<std::uint64_t> shadow_air_textures, shadow_solid_textures, shadow_cave_textures, shadow_haze_textures;
    // Where the game is: the zones of its maps ("areas/hunting/desert/area6"), the game's textures that only a zone uses (a hash
    // of the game's own texture, as packs key them, and an index into the names), and the zones that are caves.
    std::vector<std::string> shadow_zone_names, shadow_cave_zones;
    std::vector<std::pair<std::string, float>> shadow_zone_suns; // how high the sun stands in some zones, in degrees
    std::vector<std::pair<std::uint32_t, int>> shadow_zone_textures;
    std::atomic<float> shadow_strength{0.5f};
    std::atomic<float> shadow_max_object{39450.0f};
    std::atomic<float> shadow_max_piece{39450.0f};
    std::atomic<bool> shadow_show{false};
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
