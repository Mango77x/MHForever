#pragma once

// Connects a PSP runtime to the desktop host: shows the displayed framebuffer, feeds controller input and plays the
// mixed audio. Used by the game executables and the homebrew runner when a window is wanted.

#include "mhf/psp/runtime.hpp"

#include <functional>
#include <memory>

namespace mhf::link {
class Connection;
}

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

    // Connects the window to the launcher that started the game (docs/07-launcher-ux.md §3c): Escape (or the guide
    // button) pauses the game and asks the launcher for its in-game menu; the launcher's commands change the volumes,
    // resume or end the game. Without it Escape does nothing outside dialogs.
    void attach_launcher(std::unique_ptr<link::Connection> launcher);

    // Installs the runtime's host hook. `extra` runs on every poll too (frame dumps, time limits...).
    void attach(Runtime &rt, std::function<void()> extra = {});

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace mhf::psp
