#pragma once

// Desktop host built on SDL3: one window that shows the emulated screen, keyboard and gamepad input (any controller
// SDL knows, which includes everything Steam Input exposes), and a stereo audio stream (docs/07-launcher-ux.md).
// Console-independent: platforms map GamepadState to their own buttons.

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
    enum class Kind { Text, Enter, Escape, Backspace, Left, Right, Up, Down } kind = Kind::Text;
    std::string text; // Kind::Text: UTF-8
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

    // Processes pending events. Returns false once the user asked to close the window.
    bool poll();

    // Shows one frame. `pixels` holds width*height ABGR8888 values (R in the low byte), `stride` pixels per row.
    void present(const std::uint32_t *pixels, int width, int height, int stride);

    [[nodiscard]] GamepadState input() const;

    // Key events since the last call (collected by poll()).
    [[nodiscard]] std::vector<KeyEvent> take_key_events();
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

    // Queues interleaved stereo 16-bit samples for playback. Drops audio rather than growing the queue unbounded.
    void queue_audio(const std::int16_t *stereo, std::size_t frames);
    [[nodiscard]] std::size_t queued_audio_frames() const;

    struct Impl;

private:
    bool create_renderer(unsigned long long window_flags);
    std::unique_ptr<Impl> impl_;
};

} // namespace mhf::host
