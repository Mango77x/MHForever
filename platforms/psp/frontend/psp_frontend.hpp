#pragma once

// Connects a PSP runtime to the desktop host: shows the displayed framebuffer, feeds controller input and plays the
// mixed audio. Used by the game executables and the homebrew runner when a window is wanted.

#include "mhf/psp/runtime.hpp"

// host/keymap.hpp is not included here on purpose: the PSP runtime and the homebrew runners include this header without the
// SDL host's include directories. The keyboard bindings travel as a template instead.
#include <functional>
#include <memory>

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

    // The same, with the keyboard bindings the game was given (host::KeyBindings). A template, so this header never has to
    // know that type; the frontend instantiates it for the game.
    template <typename KeyBindings>
    bool open(const char *title, bool hidden, int scale, bool gpu, int render_scale, bool fullscreen,
              bool smooth_textures, int target_fps, const KeyBindings &keys) {
        const bool shown = open(title, hidden, scale, gpu, render_scale, fullscreen, smooth_textures, target_fps);
        set_keymap(keys);
        return shown;
    }

    // Installs the runtime's host hook. `extra` runs on every poll too (frame dumps, time limits...).
    void attach(Runtime &rt, std::function<void()> extra = {});

    struct Impl;

private:
    // Which physical key stands for which pad button; defined in the frontend, which does know host::KeyBindings.
    template <typename KeyBindings>
    void set_keymap(const KeyBindings &keys);

    std::unique_ptr<Impl> impl_;
};

} // namespace mhf::psp
