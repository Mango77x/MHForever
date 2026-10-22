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
    // The game is paused behind the image (the in-game menu): nothing flips any more, so the backend keeps drawing the
    // last picture with the image over it, as often as the display refreshes.
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
