#include "psp_frontend.hpp"

#include "dialog_overlay.hpp"
#include "gl_backend.hpp"
#include "host/keymap.hpp"
#include "host/padmap.hpp"
#include "host/sdl_host.hpp"
#include "overlay/menu.hpp"
#include "mhf/psp/dialog.hpp"
#include "mhf/psp/host.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cstring>
#include <array>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace mhf::psp {

// The menu's keys from a gamepad: presses, with the repeat of a held direction.
struct MenuPad {
    bool up{}, down{}, left{}, right{}, accept{}, back{}, prev{}, next{};
    std::uint64_t repeat_at_ms[4]{};
};

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
    // The in-game menu (docs/07-launcher-ux.md section 3c): Escape (or the guide button) opens it over the running game.
    overlay::Menu menu;
    std::unique_ptr<overlay::MenuRenderer> menu_renderer; // worker thread (GL path) or the emulation thread (software)
    std::optional<overlay::MenuView> menu_pending;        // for the worker (guarded by dialog_mutex)
    int overlay_pixel_w{1920}, overlay_pixel_h{1088};    // the size the menu and the dialogs are shown at (dialog_mutex)
    std::atomic<bool> menu_open{false};
    bool menu_confirming_quit{};
    std::unique_ptr<overlay::MenuRenderer> menu_layouter; // the mouse needs to know where everything is drawn
    bool menu_requested{};
    bool guide_down{};
    // The open menu between polls (step_menu).
    std::chrono::steady_clock::time_point menu_started;
    overlay::MenuLayout menu_layout;
    std::uint64_t menu_layout_version{~0ull}, menu_shown_version{~0ull};
    int menu_dragging{-1}, menu_pointer_row{-1};
    MenuPad menu_pad;
    std::vector<std::uint32_t> soft_menu; // software path: the menu's picture, 480x272
    int music_volume{100}, sfx_volume{100};
    host::KeyBindings keys; // what the Controls tab edits; the host reads them on every poll
    host::PadBindings pad;
    int capture_id{};       // the Controls row waiting for a key or a button, 0 when none
    std::filesystem::path controls_file; // where a change is written for the launcher (empty: not at all)
    // The Online tab (docs/14-rooms-and-friends.md section 3.4): the private room, read from the runtime when the menu
    // opens. `last_room` brings a code back when the player returns to private halls.
    bool online{}, room_known{};
    std::string room, last_room, room_note, server;
    // Joining a friend's room: the code as typed (keyboard) or picked (the code keyboard, for controllers).
    std::string code_typed;
    bool code_typing{}, code_keyboard{}; // the keyboard types into the code field; the on-screen one is up (controller)
    bool menu_pad_used{}; // the last key came from a controller: its hints, and the code keyboard for typing
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
                if (menu_pending) { // the menu first: it covers the game
                    menu_view = std::move(menu_pending);
                    menu_pending.reset();
                } else {
                    view = std::move(*dialog_pending);
                    dialog_pending.reset();
                }
            }
            if (menu_view) {
                if (!menu_renderer) menu_renderer = std::make_unique<overlay::MenuRenderer>(font_directory);
                int width = 1920, height = 1088;
                {
                    std::lock_guard lock(dialog_mutex);
                    width = overlay_pixel_w;
                    height = overlay_pixel_h;
                }
                std::vector<std::uint32_t> pixels;
                menu_renderer->render(*menu_view, width, height, pixels, false);
                std::lock_guard lock(overlay.mutex);
                if (menu_open) { // the menu may have been closed while this was drawn
                    overlay.pixels = std::move(pixels);
                    overlay.width = width;
                    overlay.height = height;
                    if (!overlay.visible) overlay.visible = true;
                    ++overlay.generation;
                }
                continue;
            }
            if (!dialogs) dialogs = std::make_unique<DialogOverlay>(font_directory);
            const bool visible = view.kind != DialogView::Kind::None;
            if (gpu_state == 1) {
                int width = 1920, height = 1088; // the size it is shown at: not stretched, so sharp
                {
                    std::lock_guard lock(dialog_mutex);
                    width = overlay_pixel_w;
                    height = overlay_pixel_h;
                }
                std::vector<std::uint32_t> pixels;
                if (visible) dialogs->render(view, width, height, pixels);
                std::lock_guard lock(overlay.mutex);
                overlay.pixels = std::move(pixels);
                overlay.width = width;
                overlay.height = height;
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

void Frontend::set_controls_file(const std::string &path) { impl_->controls_file = std::filesystem::path(path); }

template <typename KeyBindings, typename PadBindings>
void Frontend::set_bindings(const KeyBindings &keys, const PadBindings &pad) {
    impl_->keys = keys;
    impl_->pad = pad;
    impl_->host.set_keymap(keys);
    impl_->host.set_padmap(pad);
}

// The game's keyboard and controller bindings (mhfu --keymap, --padmap); the homebrew runner never instantiates it.
template void Frontend::set_bindings<host::KeyBindings, host::PadBindings>(const host::KeyBindings &, const host::PadBindings &);

namespace {

std::uint32_t expand(std::uint32_t v, unsigned bits) {
    return bits == 5 ? (v << 3u) | (v >> 2u) : bits == 6 ? (v << 2u) | (v >> 4u) : v | (v << 4u);
}

// Where the overlays are shown, in pixels: the window letterboxed to the PSP's 480:272, as the GL backend draws them.
void shown_size(const host::SdlHost &host, int &width, int &height) {
    int pixel_w = 0, pixel_h = 0;
    host.window_pixel_size(pixel_w, pixel_h);
    width = pixel_w;
    height = pixel_w * 272 / 480;
    if (height > pixel_h) {
        height = pixel_h;
        width = pixel_h * 480 / 272;
    }
    if (width < 480 || height < 272) { // no window yet, or a tiny one
        width = 1920;
        height = 1088;
    }
    width = std::min(width, 7680);
    height = std::min(height, 4352);
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
        shown_size(impl.host, impl.overlay_pixel_w, impl.overlay_pixel_h);
    }
    impl.dialog_cv.notify_one();
}

enum MenuId {
    kResume = 1, kQuit, kQuitYes, kQuitNo, kMusic, kSfx, kScale, kFps, kFullscreen, kSmooth, kPadStick, kRestoreKeys, kRestorePad,
    kHalls, kRoomCode, kJoinRoom, kServer
};

// The ad hoc servers the Online section offers: the launcher's list (launcher/ui/src/servers.ts, keep the two alike).
struct AdhocServerChoice {
    std::string_view host, name, location;
};
constexpr AdhocServerChoice kServers[] = {
    {"socom.cc", "Socom", "France, PPSSPP's default and the busiest"},
    {"psp.mgn.pub", "MGN", "United States, has a Monster Hunter community"},
    {"adhoc.gamesnexus.ovh", "Games Nexus", "Italy"},
    {"psi-hate.com", "Psi-Hate", "United States"},
    {"cat.wjg.jp", "Neko PSP", "Japan"},
};

// The Controls tab: one Binding row per key action (ids from kKeyBase, in the order of host::kKeyActions) and per pad button
// (from kPadBase, in the order of host::kPadActions).
constexpr int kKeyBase = 1000, kPadBase = 2000;
constexpr std::string_view kActionLabels[] = {"Cross", "Circle", "Square", "Triangle", "L", "R", "Start", "Select",
                                              "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right",
                                              "Stick Up", "Stick Down", "Stick Left", "Stick Right"};
constexpr std::size_t kPadButtons = 12; // the pad actions before the stick
constexpr std::string_view kStickChoices[] = {"left", "right", ""};

constexpr int kMaxScale = 10;                 // 1x..10x the PSP's 480x272
constexpr int kFpsChoices[] = {30, 60, 90, 120, 150, 180}; // whole multiples of the game's 30 fps

// The on-screen keyboard for a friend's room code (with a controller): these characters, then Delete and Join.
constexpr std::string_view kCodeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // Crockford base32, as room codes
constexpr int kCodeDelete = static_cast<int>(kCodeAlphabet.size()), kCodeJoin = kCodeDelete + 1;

overlay::MenuItem menu_item(overlay::MenuItem::Kind kind, int id, std::string label, int value = 0) {
    overlay::MenuItem item;
    item.kind = kind;
    item.id = id;
    item.label = std::move(label);
    item.value = value;
    return item;
}

// The sidebar's heading and the key hints, which follow what the player uses: keyboard or controller.
void menu_heading(overlay::MenuView &v, const Frontend::Impl &impl) {
    v.title = "MHForever";
    const std::size_t dash = impl.title.find(" - ");
    v.subtitle = dash == std::string::npos ? std::string() : impl.title.substr(dash + 3);
    if (impl.capture_id != 0) v.hint = "Press the new key or button  |  Backspace removes it  |  Esc cancels";
    else if (impl.code_keyboard) v.hint = "A types  |  B closes the keyboard  |  Join when the code is complete";
    else if (impl.code_typing) v.hint = "Type the code  |  Enter joins  |  Esc stops typing";
    else if (impl.menu_pad_used) v.hint = "A choose  |  B back  |  LB RB sections";
    else v.hint = "Arrows move  |  Enter choose  |  Q E sections  |  Esc back to the game";
}

// The Online section: public halls (everyone on the server, PPSSPP and PSP players too) or a private room's halls.
overlay::MenuTab menu_online(const Frontend::Impl &impl) {
    using Kind = overlay::MenuItem::Kind;
    const bool private_room = !impl.room.empty();
    overlay::MenuTab tab{"Online", {}, "Who you meet in the Gathering Hall.", private_room ? "Private" : "Public"};
    overlay::MenuItem halls = menu_item(Kind::Choice, kHalls, "", private_room ? 1 : 0);
    halls.options = {"Public halls", "Private room"};
    halls.option_details = {"Everyone on the server, PPSSPP and PSP players too.", "Only friends with your room code, on MHForever."};
    tab.items.push_back(halls);
    if (private_room) {
        overlay::MenuItem code = menu_item(Kind::Banner, kRoomCode, "Your room code");
        code.text = format_adhoc_room_code(impl.room);
        code.actions = {"Copy", "New code"};
        code.primary_action = 0;
        tab.items.push_back(code);
    }
    overlay::MenuItem join = menu_item(Kind::Text, kJoinRoom, "Join a friend's room", impl.code_typing || impl.code_keyboard ? 1 : 0);
    join.text = impl.code_typed;
    join.placeholder = impl.menu_pad_used ? "Press A to enter your friend's code" : "Type or paste a code, like K7QF-29XM";
    if (impl.code_keyboard) {
        for (const char c : kCodeAlphabet) join.keys.emplace_back(1, c);
        join.keys.emplace_back("Delete");
        join.keys.emplace_back("Join");
    }
    join.actions = {"Paste", "Join"};
    join.primary_action = adhoc_room_code(impl.code_typed).empty() ? -1 : 1;
    tab.items.push_back(join);
    overlay::MenuItem server = menu_item(Kind::Choice, kServer, "Server", -1);
    for (std::size_t i = 0; i < std::size(kServers); ++i) {
        server.options.emplace_back(kServers[i].name);
        if (kServers[i].host == impl.server) {
            server.value = static_cast<int>(i);
            server.detail = std::string(kServers[i].location);
        }
    }
    if (server.value < 0) { // one typed in the launcher
        server.value = static_cast<int>(server.options.size());
        server.options.push_back(impl.server);
    }
    tab.items.push_back(server);
    if (!impl.room_note.empty()) tab.items.push_back(menu_item(Kind::Info, 0, impl.room_note));
    return tab;
}

overlay::MenuView menu_main(const Frontend::Impl &impl) {
    using Kind = overlay::MenuItem::Kind;
    overlay::MenuView v;
    menu_heading(v, impl);

    overlay::MenuItem resume = menu_item(Kind::Button, kResume, "Back to the game");
    resume.detail = impl.online ? "The game keeps running behind this menu, so you stay in your hall or quest." : "Esc does the same.";
    overlay::MenuItem quit = menu_item(Kind::Button, kQuit, "Quit the game");
    quit.detail = "Back to the launcher. Progress since your last save is lost.";
    quit.danger = true;
    overlay::MenuTab game{"Game", {resume, quit}, v.subtitle};

    overlay::MenuTab audio{"Audio", {menu_item(Kind::Slider, kMusic, "Music", impl.music_volume),
                                      menu_item(Kind::Slider, kSfx, "Sound effects", impl.sfx_volume)},
                           "Changes are heard at once."};

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
    overlay::MenuTab video{"Video", {scale, fps,
                                      menu_item(Kind::Toggle, kFullscreen, "Fullscreen", impl.host.window_rect().fullscreen ? 1 : 0),
                                      menu_item(Kind::Toggle, kSmooth, "Smooth textures", impl.live.smooth_textures ? 1 : 0)},
                           "Changes apply at the next picture."};
    overlay::MenuTab controls{"Controls", {menu_item(Kind::Heading, 0, "Keyboard")}, "Click a key or a button, then press the new one."};
    for (std::size_t i = 0; i < host::kKeyActions.size(); ++i) {
        const int id = kKeyBase + static_cast<int>(i);
        overlay::MenuItem row = menu_item(Kind::Binding, id, std::string(kActionLabels[i]), impl.capture_id == id ? 1 : 0);
        row.text = host::key_label(*host::key_binding(impl.keys, host::kKeyActions[i]));
        controls.items.push_back(row);
    }
    controls.items.push_back(menu_item(Kind::Button, kRestoreKeys, "Restore keyboard defaults"));
    controls.items.push_back(menu_item(Kind::Heading, 0, "Controller"));
    for (std::size_t i = 0; i < kPadButtons; ++i) {
        const int id = kPadBase + static_cast<int>(i);
        overlay::MenuItem row = menu_item(Kind::Binding, id, std::string(kActionLabels[i]), impl.capture_id == id ? 1 : 0);
        row.text = host::pad_label(*host::pad_binding(impl.pad, host::kPadActions[i]));
        controls.items.push_back(row);
    }
    overlay::MenuItem stick = menu_item(Kind::Choice, kPadStick, "Analog stick");
    stick.options = {"Left stick", "Right stick", "None"};
    for (std::size_t i = 0; i < std::size(kStickChoices); ++i) {
        if (impl.pad.stick == kStickChoices[i]) stick.value = static_cast<int>(i);
    }
    controls.items.push_back(stick);
    controls.items.push_back(menu_item(Kind::Button, kRestorePad, "Restore controller defaults"));
    v.tabs = {game, audio, video, controls};
    if (impl.online) v.tabs.push_back(menu_online(impl));
    return v;
}

overlay::MenuView menu_confirm_quit(const Frontend::Impl &impl) {
    using Kind = overlay::MenuItem::Kind;
    overlay::MenuView v;
    menu_heading(v, impl);
    overlay::MenuItem yes = menu_item(Kind::Button, kQuitYes, "Quit without saving");
    yes.danger = true;
    v.tabs = {overlay::MenuTab{"Quit the game?", {menu_item(Kind::Button, kQuitNo, "Keep playing"), yes},
                               "Progress since your last save in the game is lost."}};
    return v;
}

// Applies the bindings at once and, when the launcher asked for it, writes them where it will find them: the player's
// choice then survives the session (the launcher folds the file into its settings).
void apply_bindings(Frontend::Impl &impl) {
    impl.host.set_keymap(impl.keys);
    impl.host.set_padmap(impl.pad);
    if (impl.controls_file.empty()) return;
    std::error_code ec;
    const std::filesystem::path temp = impl.controls_file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << "keymap=" << host::format_keymap(impl.keys) << "\npadmap=" << host::format_padmap(impl.pad) << "\n";
        if (impl.room_known) out << "room=" << impl.room << "\n" << "server=" << impl.server << "\n"; // the launcher offers them next time
        if (!out) return;
    }
    std::filesystem::rename(temp, impl.controls_file, ec); // replaces the previous file in one step
    if (ec) std::fprintf(stderr, "[frontend] could not save the controls: %s\n", ec.message().c_str());
}

// Shows the menu's main view again.
void show_main(Frontend::Impl &impl) { impl.menu.set(menu_main(impl)); }

// Builds the view shown again (its hints follow the keyboard or the controller).
void refresh_menu(Frontend::Impl &impl) {
    if (impl.menu_confirming_quit) impl.menu.set(menu_confirm_quit(impl));
    else impl.menu.set(menu_main(impl));
}

// Gives the game another room ("" for the public halls) and says when it applies.
void change_room(Frontend::Impl &impl, Runtime &rt, const std::string &room) {
    impl.room = room;
    if (!room.empty()) impl.last_room = room;
    rt.set_adhoc_room(room);
    const std::string where = room.empty() ? "the public halls" : "the halls of room " + format_adhoc_room_code(room);
    impl.room_note = rt.adhoc_in_hall() ? "You are in a hall now: you see " + where + " once you leave it and enter again."
                                        : "Next time you enter the Gathering Hall you see " + where + ".";
    apply_bindings(impl);
    refresh_menu(impl);
}

void set_code_typing(Frontend::Impl &impl, bool on) {
    if (impl.code_typing == on && !impl.code_keyboard) return;
    impl.code_typing = on;
    impl.code_keyboard = false;
    impl.host.set_text_input(on);
    refresh_menu(impl);
}

// The on-screen keyboard under the code field, for a controller; the keyboard types meanwhile too.
void set_code_keyboard(Frontend::Impl &impl, bool on) {
    impl.code_keyboard = on;
    impl.code_typing = false;
    impl.host.set_text_input(on);
    refresh_menu(impl);
}

// Joins the room whose code was typed or picked.
void join_typed_room(Frontend::Impl &impl, Runtime &rt) {
    const std::string code = adhoc_room_code(impl.code_typed);
    impl.code_typing = false;
    impl.code_keyboard = false;
    impl.host.set_text_input(false);
    if (code.empty()) {
        impl.room_note = "That is not a room code: 8 letters and digits, like K7QF-29XM.";
        refresh_menu(impl);
        return;
    }
    impl.code_typed.clear();
    change_room(impl, rt, code);
}

// One more character of a code: the dash comes by itself after four.
void type_code_char(std::string &typed, char c) {
    if (typed.size() >= 9) return;
    if (c != '-' && typed.size() == 4 && typed.find('-') == std::string::npos) typed.push_back('-');
    typed.push_back(c);
}

// One character less, and the dash with it.
void erase_code_char(std::string &typed) {
    if (!typed.empty()) typed.pop_back();
    if (!typed.empty() && typed.back() == '-') typed.pop_back();
}

void end_capture(Frontend::Impl &impl) {
    if (impl.capture_id == 0) return;
    impl.capture_id = 0;
    impl.host.set_capture(false);
    refresh_menu(impl);
}

// Gives `value` to `action`. No other action keeps it: one that had it takes what this one had. A pad button may hold
// several sources ("left_shoulder+left_trigger"): the new one is taken out of those lists.
template <typename Bindings, typename Lookup>
void assign_binding(Bindings &bindings, Lookup binding_of, std::string_view action, const std::string &value, bool pad) {
    std::string *slot = binding_of(bindings, action);
    if (slot == nullptr) return;
    const std::string previous = *slot;
    if (!value.empty()) {
        const std::size_t count = pad ? kPadButtons : host::kKeyActions.size();
        for (std::size_t i = 0; i < count; ++i) {
            std::string *other = binding_of(bindings, pad ? host::kPadActions[i] : host::kKeyActions[i]);
            if (other == nullptr || other == slot) continue;
            if (!pad) {
                if (*other == value) *other = previous;
                continue;
            }
            std::string rest;
            bool found = false;
            std::size_t start = 0;
            while (start <= other->size()) {
                const std::size_t plus = other->find('+', start);
                const std::string part = other->substr(start, plus == std::string::npos ? std::string::npos : plus - start);
                if (part == value) found = true;
                else if (!part.empty()) rest += (rest.empty() ? "" : "+") + part;
                if (plus == std::string::npos) break;
                start = plus + 1;
            }
            if (found) *other = rest.empty() ? previous : rest;
        }
    }
    *slot = value;
}

// What the player pressed while a Controls row waited for it.
void handle_capture(Frontend::Impl &impl) {
    const bool pad_row = impl.capture_id >= kPadBase;
    const std::size_t index = static_cast<std::size_t>(impl.capture_id - (pad_row ? kPadBase : kKeyBase));
    for (const host::CapturedInput &c : impl.host.take_captured()) {
        if (c.pad != pad_row) continue; // a keyboard row takes a key, a controller row a button
        if (!c.pad && c.name == "Escape") {
            end_capture(impl);
            return;
        }
        if (!c.pad && c.name == "F11") continue; // the game keeps it for fullscreen
        std::string value = c.name;
        if (!c.pad && (c.name == "Backspace" || c.name == "Delete")) value.clear();
        if (pad_row) {
            assign_binding(impl.pad, [](host::PadBindings &b, std::string_view a) { return host::pad_binding(b, a); }, host::kPadActions[index], value, true);
        } else {
            assign_binding(impl.keys, [](host::KeyBindings &b, std::string_view a) { return host::key_binding(b, a); }, host::kKeyActions[index], value, false);
        }
        apply_bindings(impl);
        end_capture(impl);
        return;
    }
}

// What the menu reported (a key, a click, a drag). False once the menu should close.
bool menu_handle(Frontend::Impl &impl, Runtime &rt, const overlay::Menu::Event &e) {
    switch (e.kind) {
    case overlay::Menu::Event::Kind::Back:
        if (impl.code_typing || impl.code_keyboard) {
            set_code_typing(impl, false);
            return true;
        }
        if (impl.menu_confirming_quit) {
            impl.menu_confirming_quit = false;
            show_main(impl);
            return true;
        }
        return false;
    case overlay::Menu::Event::Kind::Activated:
        switch (e.id) {
        case kResume: return false;
        case kQuit:
            impl.menu_confirming_quit = true;
            impl.menu.set(menu_confirm_quit(impl));
            return true;
        case kQuitNo:
            impl.menu_confirming_quit = false;
            show_main(impl);
            return true;
        case kQuitYes:
            rt.exit_program_from_host(0);
            return false;
        case kRestoreKeys:
            impl.keys = host::KeyBindings{};
            apply_bindings(impl);
            refresh_menu(impl);
            return true;
        case kRestorePad:
            impl.pad = host::PadBindings{};
            apply_bindings(impl);
            refresh_menu(impl);
            return true;
        case kRoomCode:
            if (e.action == 1) {
                change_room(impl, rt, new_adhoc_room_code());
            } else {
                impl.host.set_clipboard_text(format_adhoc_room_code(impl.room));
                impl.room_note = "Copied " + format_adhoc_room_code(impl.room) + ". Send it to your friends.";
                refresh_menu(impl);
            }
            return true;
        case kJoinRoom:
            if (e.action == 0) { // Paste
                const std::string clip = impl.host.clipboard_text();
                const std::string code = adhoc_room_code(clip);
                impl.code_typed = code.empty() ? clip.substr(0, 9) : format_adhoc_room_code(code);
                if (code.empty()) impl.room_note = "The clipboard holds no room code (8 letters and digits, like K7QF-29XM).";
                refresh_menu(impl);
            } else if (e.action == 1) {
                join_typed_room(impl, rt);
            } else if (impl.menu_pad_used && !impl.menu.view().pointer) { // the field, with a controller: a keyboard pops up
                set_code_keyboard(impl, !impl.code_keyboard);
            } else { // with the keyboard or the mouse: type into it
                set_code_typing(impl, !impl.code_typing);
            }
            return true;
        default:
            if (e.id >= kKeyBase && e.id < kPadBase + static_cast<int>(kPadButtons)) { // a Controls row: wait for the new key or button
                const bool same = impl.capture_id == e.id;
                impl.capture_id = same ? 0 : e.id;
                impl.host.set_capture(!same);
                refresh_menu(impl);
            }
            return true;
        }
    case overlay::Menu::Event::Kind::Key: // the on-screen keyboard of the code field
        if (e.value == kCodeJoin) {
            join_typed_room(impl, rt);
            return true;
        }
        if (e.value == kCodeDelete) erase_code_char(impl.code_typed);
        else if (e.value >= 0 && e.value < kCodeDelete) type_code_char(impl.code_typed, kCodeAlphabet[static_cast<std::size_t>(e.value)]);
        refresh_menu(impl);
        return true;
    case overlay::Menu::Event::Kind::Changed:
        switch (e.id) {
        case kMusic: impl.music_volume = e.value; rt.set_volumes(impl.music_volume, impl.sfx_volume); break;
        case kSfx: impl.sfx_volume = e.value; rt.set_volumes(impl.music_volume, impl.sfx_volume); break;
        case kFullscreen: impl.host.toggle_fullscreen(); break;
        case kSmooth: impl.live.smooth_textures = e.value != 0; break;
        case kScale:
            impl.render_scale = e.value + 1;
            impl.live.scale = impl.render_scale; // the drawing thread applies it at the next picture
            refresh_menu(impl);                  // the size shown beside the label follows
            break;
        case kServer:
            if (e.value >= 0 && e.value < static_cast<int>(std::size(kServers))) {
                const AdhocServerChoice &chosen = kServers[static_cast<std::size_t>(e.value)];
                impl.server = std::string(chosen.host);
                rt.set_adhoc_server(impl.server);
                const std::string name(chosen.name);
                impl.room_note = rt.adhoc_server_pending()
                                     ? "You are in a hall or a quest now: you move to " + name + " once you leave it."
                                     : "Next time you enter the Gathering Hall you meet the players on " + name + ".";
                if (!impl.room.empty()) impl.room_note += " Your friends have to choose " + name + " too.";
                apply_bindings(impl);
                refresh_menu(impl);
            }
            break;
        case kHalls:
            if (e.value == 0) change_room(impl, rt, "");
            else change_room(impl, rt, impl.last_room.empty() ? new_adhoc_room_code() : impl.last_room);
            break;
        case kPadStick:
            impl.pad.stick = std::string(kStickChoices[std::clamp(e.value, 0, static_cast<int>(std::size(kStickChoices)) - 1)]);
            apply_bindings(impl);
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

// The in-game menu does not pause the game: it runs behind the menu, without input, so that the other players of an
// ad hoc session keep hearing from this one (a paused PSP drops out of the quest), and the clock never jumps. `open_menu`
// puts it up, `step_menu` runs on every poll of the emulation thread until the player goes back to the game or quits,
// and `close_menu` takes it down. The menu is drawn as an overlay over the running game.
void open_menu(Frontend::Impl &impl, Runtime &rt) {
    if (impl.font_directory.empty()) impl.font_directory = rt.options().font_directory;
    if (!impl.dialog_thread.joinable() && impl.gpu_state == 1) impl.dialog_thread = std::thread([&impl] { impl.dialog_worker(); });
    impl.menu_confirming_quit = false;
    impl.online = !rt.options().adhoc_server.empty();
    impl.server = rt.adhoc_server();
    impl.code_keyboard = false;
    impl.code_typing = false;
    if (impl.online) {
        impl.room = rt.adhoc_room();
        if (!impl.room.empty()) impl.last_room = impl.room;
        impl.room_known = true;
        impl.room_note.clear();
    }
    impl.menu.set(menu_main(impl));
    impl.menu_open = true;
    {
        std::lock_guard lock(impl.overlay.mutex);
        impl.overlay.continuous = impl.gpu_state == 1;
    }
    // The drawing thread may be asleep with nothing to wait for: it has to see the menu to start drawing it.
    if (impl.gpu_state == 1) wake_gpu_backend(rt);
    (void)impl.host.take_key_events();
    impl.host.set_text_input(false);
    impl.host.set_mouse_events(true);
    impl.host.show_cursor(true);
    if (!impl.menu_layouter) impl.menu_layouter = std::make_unique<overlay::MenuRenderer>(impl.font_directory);
    impl.menu_started = std::chrono::steady_clock::now();
    impl.menu_layout_version = ~0ull;
    impl.menu_shown_version = ~0ull;
    impl.menu_dragging = -1;
    impl.menu_pointer_row = -1;
    impl.menu_pad = MenuPad{};
    impl.menu_pad.accept = impl.menu_pad.back = impl.menu_pad.prev = impl.menu_pad.next = true; // what is held now must not act in the menu
}

void close_menu(Frontend::Impl &impl) {
    impl.host.set_mouse_events(false);
    impl.host.set_text_input(false);
    impl.code_typing = false;
    impl.code_keyboard = false;
    impl.capture_id = 0;
    impl.host.set_capture(false);
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
    impl.soft_menu.clear();
    impl.dialog_version = ~0ull; // a system dialog the game opened meanwhile is drawn now
    impl.menu_requested = false;
    impl.guide_down = true; // the guide button that closed the menu is still down: it must not reopen it
}

// One poll with the menu open: its keys, the gamepad and the mouse, then its picture when it changed. False once the
// menu has closed.
bool step_menu(Frontend::Impl &impl, Runtime &rt) {
    using Key = overlay::Menu::Key;
    bool open = true;
    const bool capturing = impl.capture_id != 0; // a Controls row waits for a key or a button: nothing else acts
    std::vector<Key> keys;
    bool typed = false;
    for (const auto &k : impl.host.take_key_events()) {
        if (capturing) continue;
        using K = host::KeyEvent::Kind;
        if (impl.code_typing || impl.code_keyboard) { // typing a room code: the keyboard writes, Enter joins, Esc stops
            if (k.kind == K::Text) {
                for (const char raw : k.text) {
                    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
                    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') type_code_char(impl.code_typed, c);
                }
                typed = true;
            } else if (k.kind == K::Backspace) {
                erase_code_char(impl.code_typed);
                typed = true;
            } else if (k.kind == K::Enter) {
                join_typed_room(impl, rt);
            } else if (k.kind == K::Escape) {
                set_code_typing(impl, false);
            }
            continue;
        }
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
    if (typed) refresh_menu(impl);
    const std::size_t from_keyboard = keys.size();
    const auto since = std::chrono::steady_clock::now() - impl.menu_started;
    MenuPad &pad_state = impl.menu_pad;
    if (capturing) {
        handle_capture(impl);
        // What is held when the capture ends (the button just assigned) must not act in the menu.
        if (impl.capture_id == 0) pad_state.accept = pad_state.back = pad_state.prev = pad_state.next = pad_state.up = pad_state.down = pad_state.left = pad_state.right = true;
    } else {
        menu_pad_keys(pad_state, impl.host.input(), static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()), keys);
    }
    if (!keys.empty()) { // the hints and the way to type a code follow the last thing used
        const bool pad = keys.size() > from_keyboard;
        if (pad != impl.menu_pad_used) {
            impl.menu_pad_used = pad;
            refresh_menu(impl);
        }
    }
    for (const Key key : keys) {
        if (!menu_press(impl, rt, key)) {
            open = false;
            break;
        }
    }
    // The mouse: hover selects, a click acts, a held button drags a slider, the wheel moves a slider or the selection.
    const auto mouse = impl.host.take_mouse_events();
    if (open && !mouse.empty()) {
        overlay::MenuLayout &layout = impl.menu_layout;
        if (impl.menu.view().version != impl.menu_layout_version) {
            layout = impl.menu_layouter->layout(impl.menu.view());
            impl.menu_layout_version = impl.menu.view().version;
        }
        const host::SdlHost::WindowRect window = impl.host.window_rect();
        for (const auto &m : mouse) {
            float x = 0.0f, y = 0.0f;
            menu_point(window, m.x, m.y, x, y);
            using Hit = overlay::MenuLayout::Hit;
            const Hit hit = layout.hit(x, y);
            if (m.kind == host::MouseEvent::Kind::Move) {
                impl.menu.set_pointer(true);
                impl.menu_pointer_row = hit.kind == Hit::Kind::Row || hit.kind == Hit::Kind::Option || hit.kind == Hit::Kind::Action ? hit.index : -1;
                if (hit.kind == Hit::Kind::Key) impl.menu.hover_key(hit.sub);
                if (impl.menu_dragging >= 0 && impl.menu_dragging < static_cast<int>(layout.bars.size())) {
                    const auto &bar = layout.bars[static_cast<std::size_t>(impl.menu_dragging)];
                    if (bar.w > 0) open = menu_handle(impl, rt, impl.menu.drag_slider(impl.menu_dragging, (x - bar.x) / bar.w)) && open;
                } else if (hit.kind == Hit::Kind::Row || hit.kind == Hit::Kind::Option) {
                    impl.menu.hover_row(hit.index);
                } else if (hit.kind == Hit::Kind::Action) {
                    impl.menu.hover_row(hit.index, hit.sub);
                }
            } else if (m.kind == host::MouseEvent::Kind::Down) {
                impl.menu.set_pointer(true);
                if (impl.capture_id != 0) { // a click elsewhere gives up the assignment
                    end_capture(impl);
                    continue;
                }
                const auto &rows = impl.menu.view().items();
                const bool on_field = hit.kind == Hit::Kind::Row && hit.index >= 0 && hit.index < static_cast<int>(rows.size()) &&
                                      rows[static_cast<std::size_t>(hit.index)].kind == overlay::MenuItem::Kind::Text;
                const bool on_keypad = hit.kind == Hit::Kind::Key || hit.kind == Hit::Kind::Keypad;
                if ((impl.code_typing || impl.code_keyboard) && !on_field && !on_keypad && hit.kind != Hit::Kind::Action) {
                    set_code_typing(impl, false); // a click elsewhere stops typing
                }
                switch (hit.kind) {
                case Hit::Kind::Key: open = menu_handle(impl, rt, impl.menu.click_key(hit.sub)) && open; break;
                case Hit::Kind::Keypad: break;
                case Hit::Kind::Resume:
                case Hit::Kind::Close: open = false; break;
                case Hit::Kind::Tab: impl.menu.click_tab(hit.index); break;
                case Hit::Kind::Option: open = menu_handle(impl, rt, impl.menu.click_option(hit.index, hit.sub)) && open; break;
                case Hit::Kind::Action: open = menu_handle(impl, rt, impl.menu.click_action(hit.index, hit.sub)) && open; break;
                case Hit::Kind::Row:
                    if (on_field && (impl.code_typing || impl.code_keyboard)) break; // already typing there
                    open = menu_handle(impl, rt, impl.menu.click_row(hit.index, hit.along, hit.on_bar)) && open;
                    if (hit.on_bar) impl.menu_dragging = hit.index;
                    break;
                case Hit::Kind::None: break;
                }
            } else if (m.kind == host::MouseEvent::Kind::Up) {
                impl.menu_dragging = -1;
            } else if (m.kind == host::MouseEvent::Kind::Wheel) {
                open = menu_handle(impl, rt, impl.menu.scroll(m.wheel, impl.menu_pointer_row)) && open;
            }
            if (!open) break;
        }
        impl.menu_layout_version = ~0ull; // the rows may have moved
    }
    // The menu is drawn at the size the window shows it (letterboxed to 480:272, in pixels), so it is never stretched:
    // stretched, its text blurs. A resized window draws it again.
    bool resized = false;
    {
        std::lock_guard lock(impl.dialog_mutex);
        const int was_w = impl.overlay_pixel_w, was_h = impl.overlay_pixel_h;
        shown_size(impl.host, impl.overlay_pixel_w, impl.overlay_pixel_h);
        resized = was_w != impl.overlay_pixel_w || was_h != impl.overlay_pixel_h;
    }
    if (open && (resized || impl.menu.view().version != impl.menu_shown_version)) {
        impl.menu_shown_version = impl.menu.view().version;
        if (impl.gpu_state == 1) {
            {
                std::lock_guard lock(impl.dialog_mutex);
                impl.menu_pending = impl.menu.view();
            }
            impl.dialog_cv.notify_one();
        } else { // the software path blends it over every picture it presents
            if (!impl.menu_renderer) impl.menu_renderer = std::make_unique<overlay::MenuRenderer>(impl.font_directory);
            impl.menu_renderer->render(impl.menu.view(), 1, impl.soft_menu);
        }
    }
    if (!open) close_menu(impl);
    return open;
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
            if (impl->menu_open) {
                set_controller_state(*runtime, ControllerState{}); // the menu has the input, the game runs on without it
                step_menu(*impl, *runtime);
            } else {
                set_controller_state(*runtime, map_input(pad));
                update_dialogs(*impl, *runtime);
                if (pad.guide && !impl->guide_down) impl->menu_requested = true;
                impl->guide_down = pad.guide;
                if (impl->menu_requested && current_dialog(*runtime).kind == DialogView::Kind::None) {
                    set_controller_state(*runtime, ControllerState{}); // nothing stays pressed while the menu is up
                    open_menu(*impl, *runtime);
                }
                impl->menu_requested = false;
            }
            const std::uint64_t now = runtime->now_us();
            const std::uint64_t vblank = now / kVblankPeriodUs;
            if (vblank != impl->last_vblank && impl->gpu_state == 2) {
                impl->last_vblank = vblank;
                copy_frame(*runtime, impl->frame);
                if (impl->menu_open) {
                    blend_over(impl->frame, impl->soft_menu);
                } else if (impl->dialog_visible) {
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
