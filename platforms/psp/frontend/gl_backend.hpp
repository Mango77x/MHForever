#pragma once

// OpenGL 3.3 drawing for the GE (docs/05-runtime-hle.md §6, "hardware renderer").
//
// Runs on the GE thread: it owns the OpenGL context of the game window, draws every framebuffer the game renders
// into an upscaled render target, and presents the displayed one to the window.

#include "backend.hpp"

#include "psprecomp/guest_memory.hpp"

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
    std::uint64_t generation = 0;
};

// Creates the context on the calling thread. Returns nullptr (after logging why) when OpenGL 3.3 is unavailable.
// `scale` is the internal resolution multiplier (1 = the original 480x272). `smooth_textures` (enhancement) forces
// bilinear filtering on 3D textures. `interpolate` (enhancement) shows an in-between picture for every flip.
std::unique_ptr<gpu::Backend> make_gl_backend(SDL_Window *window, int scale, psprecomp::GuestMemory &memory,
                                              bool smooth_textures = false, OverlayImage *overlay = nullptr,
                                              bool interpolate = false);

} // namespace mhf::psp
