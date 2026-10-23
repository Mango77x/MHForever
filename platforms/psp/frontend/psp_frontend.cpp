#include "psp_frontend.hpp"

#include "dialog_overlay.hpp"
#include "gl_backend.hpp"
#include "host/keymap.hpp"
#include "host/sdl_host.hpp"
#include "overlay/menu.hpp"
#include "mhf/psp/dialog.hpp"
#include "mhf/psp/host.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

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
    double audio_owed{};        // frames of real time not yet handed to the device (keeps the fraction of a frame)
    bool audio_primed{};
    // MHF_AUDIO_LOG=1: how well the output device is fed (why the sound may click), printed every 5 s.
    struct AudioStats {
        std::uint64_t polls{}, empty{}, lost_us{}, longest_gap_us{}, dropped{}, min_queued{~0ull}, sum_queued{};
        std::uint64_t last_print_us{};
    } audio_stats;
    // The in-game menu (docs/07-launcher-ux.md section 3c): Escape (or the guide button) pauses the game and opens it.
    overlay::Menu menu;
    std::unique_ptr<overlay::MenuRenderer> menu_renderer; // worker thread (GL path) or the emulation thread (software)
    std::optional<overlay::MenuView> menu_pending;        // for the worker (guarded by dialog_mutex)
    std::atomic<bool> menu_open{false};
    bool menu_confirming_quit{};
    std::unique_ptr<overlay::MenuRenderer> menu_layouter; // the mouse needs to know where everything is drawn
    bool menu_requested{};
    bool guide_down{};
    int music_volume{100}, sfx_volume{100};
    std::string title;
    LiveSettings live; // what the menu changes while the game runs
    bool open{};
    bool gpu{};
    int render_scale{4};
    bool smooth_textures{};
    int target_fps{30};
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
            std::optional<overlay::MenuView> menu_view;
            {
                std::unique_lock lock(dialog_mutex);
                dialog_cv.wait(lock, [&] { return dialog_stop || dialog_pending.has_value() || menu_pending.has_value(); });
                if (dialog_stop) return;
                if (menu_pending) { // the menu first: the game is paused behind it
                    menu_view = std::move(menu_pending);
                    menu_pending.reset();
                } else {
                    view = std::move(*dialog_pending);
                    dialog_pending.reset();
                }
            }
            if (menu_view) {
                if (!menu_renderer) menu_renderer = std::make_unique<overlay::MenuRenderer>(font_directory);
                constexpr int kMenuScale = 4;
                std::vector<std::uint32_t> pixels;
                menu_renderer->render(*menu_view, kMenuScale, pixels, false);
                std::lock_guard lock(overlay.mutex);
                if (menu_open) { // the menu may have been closed while this was drawn
                    overlay.pixels = std::move(pixels);
                    overlay.width = overlay::MenuRenderer::width(kMenuScale);
                    overlay.height = overlay::MenuRenderer::height(kMenuScale);
                    if (!overlay.visible) overlay.visible = true;
                    ++overlay.generation;
                }
                continue;
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
                    bool smooth_textures, int target_fps) {
    host::HostOptions o;
    o.title = title;
    o.hidden = hidden;
    o.scale = scale;
    o.opengl = gpu;
    impl_->gpu = gpu;
    impl_->render_scale = render_scale;
    impl_->smooth_textures = smooth_textures;
    impl_->target_fps = target_fps;
    impl_->title = title;
    impl_->open = impl_->host.open(o);
    if (impl_->open && fullscreen && !hidden) impl_->host.toggle_fullscreen();
    return impl_->open;
}

template <typename KeyBindings>
void Frontend::set_keymap(const KeyBindings &keys) {
    impl_->host.set_keymap(keys);
}

// The game's keyboard bindings (mhfu --keymap); the homebrew runner never instantiates it.
template void Frontend::set_keymap<host::KeyBindings>(const host::KeyBindings &);

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
        using K = host::KeyEvent::Kind;
        if (!open) {
            if (k.kind == K::Escape) impl.menu_requested = true;
            continue;
        }
        switch (k.kind) {
        case K::Text: dialog_text(rt, k.text); break;
        case K::Enter: dialog_key(rt, DialogKey::Enter); break;
        case K::Escape: dialog_key(rt, DialogKey::Escape); break;
        case K::Backspace: dialog_key(rt, DialogKey::Backspace); break;
        case K::Left: dialog_key(rt, DialogKey::Left); break;
        case K::Right: dialog_key(rt, DialogKey::Right); break;
        case K::Up: dialog_key(rt, DialogKey::Up); break;
        case K::Down: dialog_key(rt, DialogKey::Down); break;
        case K::PrevTab:
        case K::NextTab: break;
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

enum MenuId { kResume = 1, kQuit, kQuitYes, kQuitNo, kMusic, kSfx, kScale, kFps, kFullscreen, kSmooth };

constexpr int kMaxScale = 10;                 // 1x..10x the PSP's 480x272
constexpr int kFpsChoices[] = {30, 60, 90, 120, 150, 180}; // whole multiples of the game's 30 fps

overlay::MenuItem menu_item(overlay::MenuItem::Kind kind, int id, std::string label, int value = 0) {
    overlay::MenuItem item;
    item.kind = kind;
    item.id = id;
    item.label = std::move(label);
    item.value = value;
    return item;
}

overlay::MenuView menu_main(const Frontend::Impl &impl) {
    using Kind = overlay::MenuItem::Kind;
    overlay::MenuView v;
    v.corner = "Esc  Resume";
    v.hint = "Arrows or mouse  |  Enter select  |  Q E tabs  |  Esc resume";

    overlay::MenuTab game{"Game", {menu_item(Kind::Button, kResume, "Resume"), menu_item(Kind::Button, kQuit, "Quit the game")}};
    game.items[1].danger = true;

    overlay::MenuTab audio{"Audio", {menu_item(Kind::Heading, 0, "Volume"), menu_item(Kind::Slider, kMusic, "Music", impl.music_volume),
                                      menu_item(Kind::Slider, kSfx, "Sound effects", impl.sfx_volume)}};

    overlay::MenuItem scale = menu_item(Kind::Choice, kScale, "Internal resolution", impl.render_scale - 1);
    for (int n = 1; n <= kMaxScale; ++n) scale.options.push_back(std::to_string(n) + "x");
    scale.detail = std::to_string(480 * impl.render_scale) + " x " + std::to_string(272 * impl.render_scale);
    overlay::MenuItem fps = menu_item(Kind::Choice, kFps, "Frame rate");
    int fps_index = 0;
    for (std::size_t i = 0; i < std::size(kFpsChoices); ++i) {
        fps.options.push_back(std::to_string(kFpsChoices[i]) + " fps");
        if (kFpsChoices[i] <= impl.target_fps) fps_index = static_cast<int>(i);
    }
    fps.value = fps_index;
    fps.detail = "The game runs at 30; the rest are in-between pictures.";
    overlay::MenuTab video{"Video", {menu_item(Kind::Heading, 0, "Picture"), scale, fps, menu_item(Kind::Heading, 0, "Display"),
                                      menu_item(Kind::Toggle, kFullscreen, "Fullscreen", impl.host.window_rect().fullscreen ? 1 : 0),
                                      menu_item(Kind::Toggle, kSmooth, "Smooth textures", impl.live.smooth_textures ? 1 : 0)}};
    v.tabs = {game, audio, video};
    return v;
}

overlay::MenuView menu_confirm_quit() {
    using Kind = overlay::MenuItem::Kind;
    overlay::MenuView v;
    v.corner = "Esc  Back";
    v.hint = "Enter select  |  Esc back";
    overlay::MenuItem yes = menu_item(Kind::Button, kQuitYes, "Quit without saving");
    yes.danger = true;
    v.tabs = {overlay::MenuTab{"Quit the game?", {menu_item(Kind::Info, 0, "Progress since your last save is lost."),
                                                    menu_item(Kind::Button, kQuitNo, "Keep playing"), yes}}};
    return v;
}

// What the menu reported (a key, a click, a drag). False once the menu should close.
bool menu_handle(Frontend::Impl &impl, Runtime &rt, const overlay::Menu::Event &e) {
    switch (e.kind) {
    case overlay::Menu::Event::Kind::Back:
        if (impl.menu_confirming_quit) {
            impl.menu_confirming_quit = false;
            impl.menu.set(menu_main(impl));
            return true;
        }
        return false;
    case overlay::Menu::Event::Kind::Activated:
        switch (e.id) {
        case kResume: return false;
        case kQuit:
            impl.menu_confirming_quit = true;
            impl.menu.set(menu_confirm_quit());
            return true;
        case kQuitNo:
            impl.menu_confirming_quit = false;
            impl.menu.set(menu_main(impl));
            return true;
        case kQuitYes:
            rt.exit_program_from_host(0);
            return false;
        default: return true;
        }
    case overlay::Menu::Event::Kind::Changed:
        switch (e.id) {
        case kMusic: impl.music_volume = e.value; rt.set_volumes(impl.music_volume, impl.sfx_volume); break;
        case kSfx: impl.sfx_volume = e.value; rt.set_volumes(impl.music_volume, impl.sfx_volume); break;
        case kFullscreen: impl.host.toggle_fullscreen(); break;
        case kSmooth: impl.live.smooth_textures = e.value != 0; break;
        case kScale:
            impl.render_scale = e.value + 1;
            impl.live.scale = impl.render_scale; // the drawing thread applies it at the next picture
            impl.menu.set(menu_main(impl));      // the size shown under the row follows
            break;
        case kFps:
            impl.target_fps = kFpsChoices[std::clamp(e.value, 0, static_cast<int>(std::size(kFpsChoices)) - 1)];
            impl.live.fps = impl.target_fps;
            break;
        default: break;
        }
        return true;
    case overlay::Menu::Event::Kind::None: break;
    }
    return true;
}

bool menu_press(Frontend::Impl &impl, Runtime &rt, overlay::Menu::Key key) { return menu_handle(impl, rt, impl.menu.press(key)); }

// A position over the window (fractions) as a point of the menu's 480x272 canvas, which sits letterboxed in the window.
void menu_point(const host::SdlHost::WindowRect &window, float nx, float ny, float &x, float &y) {
    const float picture = 480.0f / 272.0f;
    const float aspect = window.height > 0 ? static_cast<float>(window.width) / static_cast<float>(window.height) : picture;
    float fw = 1.0f, fh = 1.0f;
    if (aspect > picture) fw = picture / aspect;
    else fh = aspect / picture;
    x = (nx - (1.0f - fw) * 0.5f) / fw * 480.0f;
    y = (ny - (1.0f - fh) * 0.5f) / fh * 272.0f;
}

// The menu's keys from a gamepad: presses, with the repeat of a held direction.
struct MenuPad {
    bool up{}, down{}, left{}, right{}, accept{}, back{}, prev{}, next{};
    std::uint64_t repeat_at_ms[4]{};
};

void menu_pad_keys(MenuPad &state, const host::GamepadState &g, std::uint64_t now_ms, std::vector<overlay::Menu::Key> &out) {
    using Key = overlay::Menu::Key;
    const bool dir[4] = {g.up || g.left_y < -0.6f, g.down || g.left_y > 0.6f, g.left || g.left_x < -0.6f, g.right || g.left_x > 0.6f};
    bool *held[4] = {&state.up, &state.down, &state.left, &state.right};
    const Key keys[4] = {Key::Up, Key::Down, Key::Left, Key::Right};
    for (int i = 0; i < 4; ++i) {
        if (dir[i] && !*held[i]) {
            out.push_back(keys[i]);
            state.repeat_at_ms[i] = now_ms + 350;
        } else if (dir[i] && now_ms >= state.repeat_at_ms[i]) {
            out.push_back(keys[i]);
            state.repeat_at_ms[i] = now_ms + 70;
        }
        *held[i] = dir[i];
    }
    if (g.south && !state.accept) out.push_back(Key::Accept);
    if ((g.east || g.start || g.guide) && !state.back) out.push_back(Key::Back);
    if (g.left_shoulder && !state.prev) out.push_back(Key::PrevTab);
    if (g.right_shoulder && !state.next) out.push_back(Key::NextTab);
    state.accept = g.south;
    state.back = g.east || g.start || g.guide;
    state.prev = g.left_shoulder;
    state.next = g.right_shoulder;
}

// The in-game menu is open: the game is paused (the window stays alive, the program's clock does not count the time)
// until the player resumes or quits. The menu is drawn as an overlay over the last picture.
void run_menu(Frontend::Impl &impl, Runtime &rt) {
    using Key = overlay::Menu::Key;
    const auto started = std::chrono::steady_clock::now();
    set_controller_state(rt, ControllerState{}); // nothing stays pressed while the menu is up
    if (impl.font_directory.empty()) impl.font_directory = rt.options().font_directory;
    if (!impl.dialog_thread.joinable() && impl.gpu_state == 1) impl.dialog_thread = std::thread([&impl] { impl.dialog_worker(); });
    impl.menu_confirming_quit = false;
    impl.menu.set(menu_main(impl));
    impl.menu_open = true;
    {
        std::lock_guard lock(impl.overlay.mutex);
        impl.overlay.continuous = impl.gpu_state == 1;
    }
    // The drawing thread may be asleep with nothing to wait for: it has to see the menu to start drawing it.
    if (impl.gpu_state == 1) wake_gpu_backend(rt);
    (void)impl.host.take_key_events();
    impl.host.set_mouse_events(true);
    impl.host.show_cursor(true);
    if (!impl.menu_layouter) impl.menu_layouter = std::make_unique<overlay::MenuRenderer>(impl.font_directory);
    overlay::MenuLayout layout;
    std::uint64_t layout_version = ~0ull;
    int dragging = -1; // the slider row a held button is moving
    int pointer_row = -1;
    std::uint64_t shown_version = ~0ull;
    MenuPad pad_state;
    pad_state.accept = pad_state.back = pad_state.prev = pad_state.next = true; // what is held now must not act in the menu
    bool open = true;
    while (open) {
        if (!impl.host.poll()) {
            rt.exit_program_from_host(0);
            break;
        }
        std::vector<Key> keys;
        for (const auto &k : impl.host.take_key_events()) {
            using K = host::KeyEvent::Kind;
            switch (k.kind) {
            case K::Up: keys.push_back(Key::Up); break;
            case K::Down: keys.push_back(Key::Down); break;
            case K::Left: keys.push_back(Key::Left); break;
            case K::Right: keys.push_back(Key::Right); break;
            case K::Enter: keys.push_back(Key::Accept); break;
            case K::Escape:
            case K::Backspace: keys.push_back(Key::Back); break;
            case K::PrevTab: keys.push_back(Key::PrevTab); break;
            case K::NextTab: keys.push_back(Key::NextTab); break;
            case K::Text: break;
            }
        }
        const auto since = std::chrono::steady_clock::now() - started;
        menu_pad_keys(pad_state, impl.host.input(), static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()), keys);
        for (const Key key : keys) {
            if (!menu_press(impl, rt, key)) {
                open = false;
                break;
            }
        }
        // The mouse: hover selects, a click acts, a held button drags a slider, the wheel changes a value or the tab.
        const auto mouse = impl.host.take_mouse_events();
        if (open && !mouse.empty()) {
            if (impl.menu.view().version != layout_version) {
                layout = impl.menu_layouter->layout(impl.menu.view());
                layout_version = impl.menu.view().version;
            }
            const host::SdlHost::WindowRect window = impl.host.window_rect();
            for (const auto &m : mouse) {
                float x = 0.0f, y = 0.0f;
                menu_point(window, m.x, m.y, x, y);
                using Hit = overlay::MenuLayout::Hit;
                const Hit hit = layout.hit(x, y);
                if (m.kind == host::MouseEvent::Kind::Move) {
                    pointer_row = hit.kind == Hit::Kind::Row ? hit.index : -1;
                    if (dragging >= 0 && dragging < static_cast<int>(layout.bars.size())) {
                        const auto &bar = layout.bars[static_cast<std::size_t>(dragging)];
                        if (bar.w > 0) open = menu_handle(impl, rt, impl.menu.drag_slider(dragging, (x - bar.x) / bar.w)) && open;
                    } else if (hit.kind == Hit::Kind::Row) {
                        impl.menu.hover_row(hit.index);
                    }
                } else if (m.kind == host::MouseEvent::Kind::Down) {
                    if (hit.kind == Hit::Kind::Tab) {
                        impl.menu.click_tab(hit.index);
                    } else if (hit.kind == Hit::Kind::Row) {
                        open = menu_handle(impl, rt, impl.menu.click_row(hit.index, hit.along, hit.on_bar)) && open;
                        if (hit.on_bar) dragging = hit.index;
                    }
                } else if (m.kind == host::MouseEvent::Kind::Up) {
                    dragging = -1;
                } else if (m.kind == host::MouseEvent::Kind::Wheel) {
                    open = menu_handle(impl, rt, impl.menu.scroll(m.wheel, pointer_row)) && open;
                }
                if (!open) break;
            }
            layout_version = ~0ull; // the rows may have moved
        }
        if (open && impl.menu.view().version != shown_version) {
            shown_version = impl.menu.view().version;
            if (impl.gpu_state == 1) {
                {
                    std::lock_guard lock(impl.dialog_mutex);
                    impl.menu_pending = impl.menu.view();
                }
                impl.dialog_cv.notify_one();
            } else {
                if (!impl.menu_renderer) impl.menu_renderer = std::make_unique<overlay::MenuRenderer>(impl.font_directory);
                std::vector<std::uint32_t> pixels;
                impl.menu_renderer->render(impl.menu.view(), 1, pixels);
                copy_frame(rt, impl.frame);
                blend_over(impl.frame, pixels);
                impl.host.present(impl.frame.data(), 480, 272, 480);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    impl.host.set_mouse_events(false);
    impl.menu_open = false;
    {
        std::lock_guard lock(impl.overlay.mutex);
        impl.overlay.continuous = false;
        impl.overlay.visible = false;
        impl.overlay.pixels.clear();
        impl.overlay.width = 0;
        ++impl.overlay.generation;
    }
    {
        std::lock_guard lock(impl.dialog_mutex);
        impl.menu_pending.reset();
    }
    rt.skip_time(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count()));
    impl.last_audio_us = rt.now_us();
    impl.audio_owed = 0.0;
    impl.audio_primed = false; // the device ran dry: fill the cushion again
    impl.menu_requested = false;
    impl.guide_down = true; // the guide button that closed the menu is still down: it must not reopen it
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
    impl->music_volume = rt.options().music_volume;
    impl->sfx_volume = rt.options().sfx_volume;
    impl->live.smooth_textures = impl->smooth_textures;
    if (impl->gpu && impl->open) {
        set_gpu_backend_factory(rt, [impl, runtime]() -> std::unique_ptr<gpu::Backend> {
            auto backend = make_gl_backend(static_cast<SDL_Window *>(impl->host.window_handle()), impl->render_scale,
                                           runtime->memory(), impl->smooth_textures, &impl->overlay, impl->target_fps,
                                           &impl->live);
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
            const host::GamepadState pad = impl->host.input();
            set_controller_state(*runtime, map_input(pad));
            update_dialogs(*impl, *runtime);
            if (pad.guide && !impl->guide_down) impl->menu_requested = true;
            impl->guide_down = pad.guide;
            if (impl->menu_requested && current_dialog(*runtime).kind == DialogView::Kind::None) run_menu(*impl, *runtime);
            impl->menu_requested = false;
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
                // The time since the last poll in frames, keeping the fraction: truncating it every few milliseconds
                // starved the device by ~0.2 % (a click every time its queue ran empty). The queue is steered toward
                // kAudioCushion frames by a few frames per poll, so the device clock and ours cannot drift apart.
                constexpr std::int64_t kAudioCushion = 1764; // 40 ms
                if (!impl->audio_primed) { // start with the cushion filled with silence
                    impl->audio_primed = true;
                    const std::vector<std::int16_t> silence(static_cast<std::size_t>(kAudioCushion) * 2, 0);
                    impl->host.queue_audio(silence.data(), static_cast<std::size_t>(kAudioCushion));
                }
                impl->audio_owed += static_cast<double>(elapsed) * 44100.0 / 1'000'000.0;
                const std::uint64_t wanted = static_cast<std::uint64_t>(impl->audio_owed);
                const std::int64_t queued_now = static_cast<std::int64_t>(impl->host.queued_audio_frames());
                const std::int64_t steer = std::clamp((kAudioCushion - queued_now) / 128, std::int64_t{-2}, std::int64_t{2});
                const std::int64_t target = std::max<std::int64_t>(static_cast<std::int64_t>(wanted) + steer, 0);
                const auto frames = static_cast<std::size_t>(std::min<std::int64_t>(target, 4096));
                impl->audio_owed -= static_cast<double>(wanted);
                if (wanted > 4096u) impl->audio_owed = 0.0; // a long stall: the rest of that time is gone
                static const bool log_audio = std::getenv("MHF_AUDIO_LOG") != nullptr;
                if (log_audio) {
                    auto &st = impl->audio_stats;
                    const std::uint64_t queued = impl->host.queued_audio_frames();
                    ++st.polls;
                    st.empty += queued == 0 ? 1u : 0u;
                    st.min_queued = std::min(st.min_queued, queued);
                    st.sum_queued += queued;
                    st.longest_gap_us = std::max(st.longest_gap_us, elapsed);
                    if (wanted > frames + 2u) st.lost_us += (wanted - frames) * 1'000'000u / 44100u; // time never mixed
                    if (queued > 44100u / 5u) ++st.dropped;                                       // chunk thrown away
                    if (now - st.last_print_us >= 5'000'000u) {
                        if (st.last_print_us != 0 && st.polls > 0) {
                            std::fprintf(stderr,
                                         "[audio] 5 s: polls %llu, device queue empty at %llu, queue min %llu / avg %llu frames, "
                                         "longest gap %llu ms, mixed-away time %llu ms, chunks dropped %llu\n",
                                         static_cast<unsigned long long>(st.polls), static_cast<unsigned long long>(st.empty),
                                         static_cast<unsigned long long>(st.min_queued == ~0ull ? 0 : st.min_queued),
                                         static_cast<unsigned long long>(st.sum_queued / st.polls),
                                         static_cast<unsigned long long>(st.longest_gap_us / 1000u),
                                         static_cast<unsigned long long>(st.lost_us / 1000u),
                                         static_cast<unsigned long long>(st.dropped));
                        }
                        st = {};
                        st.min_queued = ~0ull;
                        st.last_print_us = now;
                    }
                }
                std::vector<std::int16_t> mixed(frames * 2);
                const std::size_t produced = mix_audio(*runtime, mixed.data(), frames);
                if (produced > 0) impl->host.queue_audio(mixed.data(), produced);
            }
        }
        if (extra) extra();
    });
}

} // namespace mhf::psp
