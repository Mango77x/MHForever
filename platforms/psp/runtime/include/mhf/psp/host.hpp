#pragma once

// What the host (window, input, audio) exchanges with the PSP runtime.

#include "mhf/psp/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace mhf::psp {

namespace gpu {
class Backend;
} // namespace gpu

// Framebuffer the guest asked the display to scan out (sceDisplaySetFrameBuf).
struct DisplayState {
    std::uint32_t framebuffer{};   // guest address, 0 = display off
    std::uint32_t stride{512};     // pixels per line
    std::uint32_t pixel_format{3}; // 0 = 565, 1 = 5551, 2 = 4444, 3 = 8888
    std::uint64_t frames{};        // number of SetFrameBuf calls
};

// PSP button bits (sceCtrl).
namespace buttons {
inline constexpr std::uint32_t kSelect = 0x000001u;
inline constexpr std::uint32_t kStart = 0x000008u;
inline constexpr std::uint32_t kUp = 0x000010u;
inline constexpr std::uint32_t kRight = 0x000020u;
inline constexpr std::uint32_t kDown = 0x000040u;
inline constexpr std::uint32_t kLeft = 0x000080u;
inline constexpr std::uint32_t kLTrigger = 0x000100u;
inline constexpr std::uint32_t kRTrigger = 0x000200u;
inline constexpr std::uint32_t kTriangle = 0x001000u;
inline constexpr std::uint32_t kCircle = 0x002000u;
inline constexpr std::uint32_t kCross = 0x004000u;
inline constexpr std::uint32_t kSquare = 0x008000u;
inline constexpr std::uint32_t kHome = 0x010000u;
} // namespace buttons

struct ControllerState {
    std::uint32_t buttons{};
    std::uint8_t lx{128};
    std::uint8_t ly{128};
};

[[nodiscard]] const DisplayState &display_state(Runtime &rt);
void set_controller_state(Runtime &rt, const ControllerState &state);

// Mixes up to `frames` stereo frames of what the game has output on its audio channels into `out` (interleaved
// 16-bit, 44.1 kHz) and returns how many frames were available. Missing data is silence.
std::size_t mix_audio(Runtime &rt, std::int16_t *out, std::size_t frames);

// Hardware rendering: the factory runs once on the GE thread (where the backend then lives) before the first display
// list. Returning nullptr keeps the software renderer. While a backend is active it also presents the displayed
// framebuffer (sceDisplaySetFrameBuf). Returns whether a backend is currently active.
void set_gpu_backend_factory(Runtime &rt, std::function<std::unique_ptr<gpu::Backend>()> factory);
[[nodiscard]] bool gpu_backend_active(Runtime &rt);
// Stops display-list processing and destroys the backend (before its window goes away).
void stop_gpu_backend(Runtime &rt);

// Duration of one PSP video frame (59.94 Hz) in microseconds.
inline constexpr std::uint64_t kVblankPeriodUs = 16683u;

} // namespace mhf::psp
