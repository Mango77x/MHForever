#pragma once

// Desktop host built on SDL3: one window that shows the emulated screen, keyboard and gamepad input (any controller
// SDL knows, which includes everything Steam Input exposes), and a stereo audio stream (docs/07-launcher-ux.md).
// Console-independent: platforms map GamepadState to their own buttons.

#include "host/keymap.hpp"
#include "host/padmap.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mhf::host {

struct GamepadState {
    bool south{}, east{}, west{}, north{};      // face buttons (A/B/X/Y on Xbox layout: south = A)
    bool left_shoulder{}, right_shoulder{};
    bool start{}, select{}, guide{};
    bool up{}, down{}, left{}, right{};
    float left_x{}, left_y{};                   // -1..1
    float right_x{}, right_y{};
};

// Keyboard input for text and menus (dialogs drawn by the platform), in the order it happened.
struct KeyEvent {
    enum class Kind { Text, Enter, Escape, Backspace, Left, Right, Up, Down, PrevTab, NextTab } kind = Kind::Text;
    std::string text; // Kind::Text: UTF-8
};

// The mouse over the window's drawing area, for menus drawn by the game. Positions are fractions of the window (0..1).
struct MouseEvent {
    enum class Kind { Move, Down, Up, Wheel } kind = Kind::Move;
    float x{}, y{};
    int wheel{}; // Wheel: +1 up, -1 down
};

struct HostOptions {
    const char *title = "MH-Forever";
    int width = 480;   // emulated screen size
    int height = 272;
    int scale = 2;     // initial window scale
    bool hidden = false; // no visible window (tests, servers)
    int audio_rate = 44100;
    bool opengl = false; // window for an OpenGL 3.3 core context (the caller draws); no 2D renderer until needed
};

class SdlHost {
public:
    SdlHost();
    ~SdlHost();
    SdlHost(const SdlHost &) = delete;
    SdlHost &operator=(const SdlHost &) = delete;

    // Opens the window and audio. Returns false (and logs why) when SDL cannot start.
    bool open(const HostOptions &options);

    // Which physical key, and which button of a controller, stands for which button of the emulated pad. The defaults are
    // KeyBindings{} and PadBindings{}; the launcher passes the player's choices, see docs/07-launcher-ux.md section 5.
    void set_keymap(const KeyBindings &bindings);
    void set_padmap(const PadBindings &bindings);

    // Processes pending events. Returns false once the user asked to close the window.
    bool poll();

    // Shows one frame. `pixels` holds width*height ABGR8888 values (R in the low byte), `stride` pixels per row.
    void present(const std::uint32_t *pixels, int width, int height, int stride);

    [[nodiscard]] GamepadState input() const;

    // Key events since the last call (collected by poll()).
    [[nodiscard]] std::vector<KeyEvent> take_key_events();
    // Mouse events since the last call, collected by poll() only while set_mouse_events(true) (left button and wheel).
    [[nodiscard]] std::vector<MouseEvent> take_mouse_events();
    void set_mouse_events(bool on);
    // Shows or hides the mouse cursor over the window.
    void show_cursor(bool show);
    // Text entry (IME, typed characters). While it is on, the keyboard no longer acts as a gamepad.
    void set_text_input(bool on);

    // The SDL_Window (for OpenGL contexts); nullptr before open().
    [[nodiscard]] void *window_handle() const;
    // Toggles borderless fullscreen.
    void toggle_fullscreen();
    // Where the window's drawing area is on the desktop (screen pixels) and whether it is fullscreen.
    struct WindowRect {
        int x{}, y{}, width{}, height{};
        bool fullscreen{};
    };
    [[nodiscard]] WindowRect window_rect() const;
    // Brings the window to the front and gives it the keyboard focus.
    void raise();

    // Queues interleaved stereo 16-bit samples for playback. Drops audio rather than growing the queue unbounded.
    void queue_audio(const std::int16_t *stereo, std::size_t frames);
    [[nodiscard]] std::size_t queued_audio_frames() const;

    struct Impl;

private:
    bool create_renderer(unsigned long long window_flags);
    std::unique_ptr<Impl> impl_;
};

} // namespace mhf::host
