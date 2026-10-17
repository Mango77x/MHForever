#include "psp_frontend.hpp"

#include "gl_backend.hpp"
#include "host/sdl_host.hpp"
#include "mhf/psp/host.hpp"

#include <atomic>
#include <cstdio>

#include <algorithm>
#include <cstring>
#include <array>
#include <cmath>
#include <vector>

namespace mhf::psp {

struct Frontend::Impl {
    host::SdlHost host;
    std::vector<std::uint32_t> frame = std::vector<std::uint32_t>(480 * 272);
    std::uint64_t last_vblank{};
    std::uint64_t last_audio_us{};
    bool open{};
    bool gpu{};
    int render_scale{4};
    std::atomic<int> gpu_state{0};
    Runtime *runtime{}; // 0 not started, 1 active, 2 unavailable (software presentation)
};

Frontend::Frontend() : impl_(std::make_unique<Impl>()) {}
Frontend::~Frontend() {
    // The GE thread draws into our window: stop it before the window is destroyed.
    if (impl_->runtime != nullptr && impl_->gpu) stop_gpu_backend(*impl_->runtime);
}

bool Frontend::open(const char *title, bool hidden, int scale, bool gpu, int render_scale) {
    host::HostOptions o;
    o.title = title;
    o.hidden = hidden;
    o.scale = scale;
    o.opengl = gpu;
    impl_->gpu = gpu;
    impl_->render_scale = render_scale;
    impl_->open = impl_->host.open(o);
    return impl_->open;
}

namespace {

std::uint32_t expand(std::uint32_t v, unsigned bits) {
    return bits == 5 ? (v << 3u) | (v >> 2u) : bits == 6 ? (v << 2u) | (v >> 4u) : v | (v << 4u);
}

void copy_frame(Runtime &rt, std::vector<std::uint32_t> &out) {
    const auto &d = display_state(rt);
    if (d.framebuffer == 0) {
        std::fill(out.begin(), out.end(), 0xFF000000u);
        return;
    }
    auto &mem = rt.memory();
    const std::uint32_t bpp = d.pixel_format == 3 ? 4u : 2u;
    const std::uint8_t *base = mem.raw_pointer(d.framebuffer, static_cast<std::size_t>(d.stride) * 271u * bpp + 480u * bpp);
    if (base == nullptr) {
        std::fill(out.begin(), out.end(), 0xFF000000u);
        return;
    }
    for (std::uint32_t y = 0; y < 272; ++y) {
        const std::uint8_t *row = base + static_cast<std::size_t>(y) * d.stride * bpp;
        for (std::uint32_t x = 0; x < 480; ++x) {
            std::uint32_t c;
            if (d.pixel_format == 3) {
                std::memcpy(&c, row + x * 4u, 4);
            } else {
                std::uint16_t p16;
                std::memcpy(&p16, row + x * 2u, 2);
                const std::uint32_t p = p16;
                std::uint32_t r, g, b;
                if (d.pixel_format == 0) { r = expand(p & 31u, 5); g = expand((p >> 5u) & 63u, 6); b = expand((p >> 11u) & 31u, 5); }
                else if (d.pixel_format == 1) { r = expand(p & 31u, 5); g = expand((p >> 5u) & 31u, 5); b = expand((p >> 10u) & 31u, 5); }
                else { r = expand(p & 15u, 4); g = expand((p >> 4u) & 15u, 4); b = expand((p >> 8u) & 15u, 4); }
                c = r | (g << 8u) | (b << 16u);
            }
            out[y * 480 + x] = c | 0xFF000000u;
        }
    }
}

ControllerState map_input(const host::GamepadState &g) {
    ControllerState s;
    auto set = [&](bool on, std::uint32_t bit) { if (on) s.buttons |= bit; };
    set(g.south, buttons::kCross);
    set(g.east, buttons::kCircle);
    set(g.west, buttons::kSquare);
    set(g.north, buttons::kTriangle);
    set(g.left_shoulder, buttons::kLTrigger);
    set(g.right_shoulder, buttons::kRTrigger);
    set(g.start, buttons::kStart);
    set(g.select, buttons::kSelect);
    set(g.up, buttons::kUp);
    set(g.down, buttons::kDown);
    set(g.left, buttons::kLeft);
    set(g.right, buttons::kRight);
    auto axis = [](float v) { return static_cast<std::uint8_t>(std::clamp(128.0f + v * 127.0f, 0.0f, 255.0f)); };
    s.lx = axis(g.left_x);
    s.ly = axis(g.left_y);
    return s;
}

} // namespace

void Frontend::attach(Runtime &rt, std::function<void()> extra) {
    Impl *impl = impl_.get();
    Runtime *runtime = &rt;
    impl->runtime = runtime;
    if (impl->gpu && impl->open) {
        set_gpu_backend_factory(rt, [impl, runtime]() -> std::unique_ptr<gpu::Backend> {
            auto backend = make_gl_backend(static_cast<SDL_Window *>(impl->host.window_handle()), impl->render_scale,
                                           runtime->memory());
            if (!backend) std::fprintf(stderr, "[frontend] no OpenGL 3.3: using the software renderer\n");
            impl->gpu_state = backend ? 1 : 2;
            return backend;
        });
    } else {
        impl->gpu_state = 2;
    }
    rt.kernel_host_poll([impl, runtime, extra]() {
        if (impl->open) {
            if (!impl->host.poll()) {
                runtime->exit_program_from_host(0);
                return;
            }
            set_controller_state(*runtime, map_input(impl->host.input()));
            const std::uint64_t now = runtime->now_us();
            const std::uint64_t vblank = now / kVblankPeriodUs;
            if (vblank != impl->last_vblank && impl->gpu_state == 2) {
                impl->last_vblank = vblank;
                copy_frame(*runtime, impl->frame);
                impl->host.present(impl->frame.data(), 480, 272, 480);
            }
            // Audio: hand the device what the game produced since the last poll.
            const std::uint64_t elapsed = now - impl->last_audio_us;
            if (elapsed >= 5000) {
                impl->last_audio_us = now;
                const auto frames = static_cast<std::size_t>(std::min<std::uint64_t>(elapsed * 44100u / 1'000'000u, 4096u));
                std::vector<std::int16_t> mixed(frames * 2);
                mix_audio(*runtime, mixed.data(), frames);
                impl->host.queue_audio(mixed.data(), frames);
            }
        }
        if (extra) extra();
    });
}

} // namespace mhf::psp
