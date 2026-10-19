#include "psp_frontend.hpp"

#include "dialog_overlay.hpp"
#include "gl_backend.hpp"
#include "host/sdl_host.hpp"
#include "mhf/psp/dialog.hpp"
#include "mhf/psp/host.hpp"

#include <atomic>
#include <cstdio>

#include <algorithm>
#include <cstring>
#include <array>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
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
    bool smooth_textures{};
    std::atomic<int> gpu_state{0};
    // System dialogs drawn over the game. Drawing one takes a few tens of milliseconds at 4x, so a worker thread
    // does it; the emulation thread only hands over the latest view.
    std::unique_ptr<DialogOverlay> dialogs;
    OverlayImage overlay;                    // GL path (GE thread reads it)
    std::mutex soft_mutex;
    std::vector<std::uint32_t> soft_overlay; // software path, 480x272
    std::uint64_t dialog_version{~0ull};
    bool dialog_visible{};
    std::thread dialog_thread;
    std::mutex dialog_mutex;
    std::condition_variable dialog_cv;
    std::optional<DialogView> dialog_pending;
    bool dialog_stop{};
    std::filesystem::path font_directory;

    void dialog_worker() {
        for (;;) {
            DialogView view;
            {
                std::unique_lock lock(dialog_mutex);
                dialog_cv.wait(lock, [&] { return dialog_stop || dialog_pending.has_value(); });
                if (dialog_stop) return;
                view = std::move(*dialog_pending);
                dialog_pending.reset();
            }
            if (!dialogs) dialogs = std::make_unique<DialogOverlay>(font_directory);
            const bool visible = view.kind != DialogView::Kind::None;
            if (gpu_state == 1) {
                constexpr int kScale = 4; // 1920x1088: sharp at 1080p, scaled smoothly elsewhere
                std::vector<std::uint32_t> pixels;
                if (visible) dialogs->render(view, kScale, pixels);
                std::lock_guard lock(overlay.mutex);
                overlay.pixels = std::move(pixels);
                overlay.width = DialogOverlay::width(kScale);
                overlay.height = DialogOverlay::height(kScale);
                overlay.visible = visible;
                ++overlay.generation;
            } else {
                std::vector<std::uint32_t> pixels;
                if (visible) dialogs->render(view, 1, pixels);
                std::lock_guard lock(soft_mutex);
                soft_overlay = std::move(pixels);
            }
        }
    }
    Runtime *runtime{}; // 0 not started, 1 active, 2 unavailable (software presentation)
};

Frontend::Frontend() : impl_(std::make_unique<Impl>()) {}
Frontend::~Frontend() {
    if (impl_->dialog_thread.joinable()) {
        {
            std::lock_guard lock(impl_->dialog_mutex);
            impl_->dialog_stop = true;
        }
        impl_->dialog_cv.notify_all();
        impl_->dialog_thread.join();
    }
    // The GE thread draws into our window: stop it before the window is destroyed.
    if (impl_->runtime != nullptr && impl_->gpu) stop_gpu_backend(*impl_->runtime);
}

bool Frontend::open(const char *title, bool hidden, int scale, bool gpu, int render_scale, bool fullscreen,
                    bool smooth_textures) {
    host::HostOptions o;
    o.title = title;
    o.hidden = hidden;
    o.scale = scale;
    o.opengl = gpu;
    impl_->gpu = gpu;
    impl_->render_scale = render_scale;
    impl_->smooth_textures = smooth_textures;
    impl_->open = impl_->host.open(o);
    if (impl_->open && fullscreen && !hidden) impl_->host.toggle_fullscreen();
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

void blend_over(std::vector<std::uint32_t> &frame, const std::vector<std::uint32_t> &overlay) {
    for (std::size_t i = 0; i < frame.size() && i < overlay.size(); ++i) {
        const std::uint32_t o = overlay[i], a = o >> 24u;
        if (a == 0) continue;
        const std::uint32_t f = frame[i], inv = 255u - a;
        std::uint32_t out = 0xFF000000u;
        for (unsigned shift = 0; shift < 24; shift += 8) {
            const std::uint32_t c = ((o >> shift) & 0xFFu) + (((f >> shift) & 0xFFu) * inv + 127u) / 255u;
            out |= std::min(c, 255u) << shift;
        }
        frame[i] = out;
    }
}

// Keys, text and the redraw of the system dialogs (mhf/psp/dialog.hpp).
void update_dialogs(Frontend::Impl &impl, Runtime &rt) {
    const bool open = current_dialog(rt).kind != DialogView::Kind::None;
    for (const auto &k : impl.host.take_key_events()) {
        if (!open) continue;
        using K = host::KeyEvent::Kind;
        switch (k.kind) {
        case K::Text: dialog_text(rt, k.text); break;
        case K::Enter: dialog_key(rt, DialogKey::Enter); break;
        case K::Escape: dialog_key(rt, DialogKey::Escape); break;
        case K::Backspace: dialog_key(rt, DialogKey::Backspace); break;
        case K::Left: dialog_key(rt, DialogKey::Left); break;
        case K::Right: dialog_key(rt, DialogKey::Right); break;
        case K::Up: dialog_key(rt, DialogKey::Up); break;
        case K::Down: dialog_key(rt, DialogKey::Down); break;
        }
    }
    impl.host.set_text_input(dialog_wants_text(rt));
    const DialogView &view = current_dialog(rt);
    if (view.version == impl.dialog_version) return;
    impl.dialog_version = view.version;
    impl.dialog_visible = view.kind != DialogView::Kind::None;
    if (!impl.dialog_thread.joinable()) {
        impl.font_directory = rt.options().font_directory;
        impl.dialog_thread = std::thread([&impl] { impl.dialog_worker(); });
    }
    {
        std::lock_guard lock(impl.dialog_mutex);
        impl.dialog_pending = view; // only the newest view matters
    }
    impl.dialog_cv.notify_one();
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
    // The PSP has no right stick: it drives the D-pad, which is what the game uses for its camera (docs/07-launcher-ux.md §5).
    constexpr float kRightStickThreshold = 0.5f;
    set(g.right_y < -kRightStickThreshold, buttons::kUp);
    set(g.right_y > kRightStickThreshold, buttons::kDown);
    set(g.right_x < -kRightStickThreshold, buttons::kLeft);
    set(g.right_x > kRightStickThreshold, buttons::kRight);
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
                                           runtime->memory(), impl->smooth_textures, &impl->overlay);
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
            update_dialogs(*impl, *runtime);
            const std::uint64_t now = runtime->now_us();
            const std::uint64_t vblank = now / kVblankPeriodUs;
            if (vblank != impl->last_vblank && impl->gpu_state == 2) {
                impl->last_vblank = vblank;
                copy_frame(*runtime, impl->frame);
                if (impl->dialog_visible) {
                    std::lock_guard lock(impl->soft_mutex);
                    blend_over(impl->frame, impl->soft_overlay);
                }
                impl->host.present(impl->frame.data(), 480, 272, 480);
            }
            // Audio: hand the device the output for the real time elapsed since the last poll.
            const std::uint64_t elapsed = now - impl->last_audio_us;
            if (elapsed >= 5000) {
                impl->last_audio_us = now;
                const auto frames = static_cast<std::size_t>(std::min<std::uint64_t>(elapsed * 44100u / 1'000'000u, 4096u));
                std::vector<std::int16_t> mixed(frames * 2);
                const std::size_t produced = mix_audio(*runtime, mixed.data(), frames);
                if (produced > 0) impl->host.queue_audio(mixed.data(), produced);
            }
        }
        if (extra) extra();
    });
}

} // namespace mhf::psp
