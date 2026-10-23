#pragma once

// Connects a PSP runtime to the desktop host: shows the displayed framebuffer, feeds controller input and plays the
// mixed audio. Used by the game executables and the homebrew runner when a window is wanted.

#include "mhf/psp/runtime.hpp"

// host/keymap.hpp and host/padmap.hpp are not included here on purpose: the PSP runtime and the homebrew runners include this
// header without the SDL host's include directories. The keyboard and controller bindings travel as templates instead.
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

    // The same, with the keyboard and controller bindings the game was given (host::KeyBindings, host::PadBindings). A
    // template, so this header never has to know those types; the frontend instantiates it for the game.
    template <typename KeyBindings, typename PadBindings>
    bool open(const char *title, bool hidden, int scale, bool gpu, int render_scale, bool fullscreen,
              bool smooth_textures, int target_fps, const KeyBindings &keys, const PadBindings &pad) {
        const bool shown = open(title, hidden, scale, gpu, render_scale, fullscreen, smooth_textures, target_fps);
        set_bindings(keys, pad);
        return shown;
    }

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
