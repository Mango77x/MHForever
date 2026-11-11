// Freedom Unite host: runs the recompiled game against the player's own ISO (docs/06-mhfu-module.md).
//
// usage: mhfu --iso <image.iso> --exe <decrypted executable> [--trace] [--memstick <dir>]
//             [--window] [--auto-dialogs] [--dump-frames <dir> [--dump-every N] [--dump-from S]] [--seconds S] [--quiet-overlays]
//             [--input "<seconds>:<button>,..."] [--name <text>] [--software] [--scale N] [--fullscreen]
//             [--smooth-textures] [--texture-pack <folder> [--pack-mipmaps] [--anisotropy 1|2|4|8|16]] [--fps 30..180] [--hud-size 50-100] [--widescreen] [--true-weapon-values] [--fast-supply-box]
//             [--smithy-browse [--smithy-layout]]
//             [--analog-camera [--analog-camera-turn-speed 50-200] [--analog-camera-tilt-speed 50-200]]
//             [--mouse-camera [--mouse-camera-speed 25-300]]
//             [--horn-songs [--horn-songs-place next-to-sheet|top-middle|under-names|bottom-left|custom:X,Y]]
//             [--projected-shadows [--no-whole-map-shadows] [--shadow-quality 0-2] [--shadow-darkness 0-100]]
//             [--music-volume 0-100] [--sfx-volume 0-100]
//             [--language en|fr|de|es|it|jp]
//             [--keymap "south=KeyK,east=KeyL,..."] [--padmap "south=south,left_shoulder=left_shoulder+left_trigger,..."]
//             [--controls-file <path>]
//             [--adhoc-server host[:port]] [--adhoc-nick <name>] [--adhoc-mac AA:BB:CC:DD:EE:FF] [--adhoc-room CODE]
// The executable supplies the initial data sections. The launcher's installer prepares it from the player's own disc image
// (launcher/native/install/); developer builds can also point --exe at a file that matches region.toml's dev_executable.

#include "mhf/psp/frame_dump.hpp"
#include "mhf/psp/generated.hpp"
#include "mhf/psp/host.hpp"
#include "mhf/psp/runtime.hpp"
#include "iso9660.hpp"
#include "param_sfo.hpp"

#include "host/keymap.hpp"
#include "host/padmap.hpp"

#if !__has_include("mhfu_region_data.hpp")
#error "mhfu_region_data.hpp is missing from MHF_MHFU_GENERATED: re-run games/mhfu/tools/prepare.py"
#endif
#include "mhfu_region_data.hpp" // generated from this release's regions/<disc>/symbols.toml [data]

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>

#if defined(MHF_HAS_FRONTEND)
#include "horn_songs.hpp"
#include "psp_frontend.hpp"
#include "shadow_textures.hpp"
#endif
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The PSP's system languages, by the value sceUtilityGetSystemParamInt(8) reports for each.
constexpr std::pair<std::string_view, int> kLanguages[] = {
    {"jp", 0}, {"en", 1}, {"fr", 2}, {"es", 3}, {"de", 4}, {"it", 5},
};

// Where the boot's language screen waits: the region map's data entry (`mh_language_screen_stage` in its
// symbols.toml), generated into mhfu_region_data.hpp. Never a number here — the same address means something else in
// another build, so pressing at it there could answer the wrong screen. A release whose map does not name it (the
// Japanese one, single-language and with no such screen) only gets the language preselected, with no press at all.
constexpr std::optional<std::uint32_t> kLanguageScreenStageAddress =
    mhf::psp::generated::mhfu_region_data::mh_language_screen_stage;
constexpr std::uint32_t kLanguageScreenStageValue = 7u;

namespace mhf::psp::generated {
std::span<const Program *const> mhfu_programs();
}

namespace {

using mhf::psp::Context;
using mhf::psp::Runtime;
namespace region = mhf::psp::generated::mhfu_region_data;

bool g_log_overlays = true;

// The "0" the element value formats print after the value, for every language a release can load; each release maps
// its own languages. The chosen language's menu text is loaded at run time, each language at its own place, so only
// that language's entries hold the formats.
constexpr std::optional<std::uint32_t> kElementValueFormatZeros[] = {
    region::mh_element_value_format_zero_en, region::mh_dual_element_value_zero_en,
    region::mh_element_value_format_zero_fr, region::mh_dual_element_value_zero_fr,
    region::mh_element_value_format_zero_de, region::mh_dual_element_value_zero_de,
    region::mh_element_value_format_zero_es, region::mh_dual_element_value_zero_es,
    region::mh_element_value_format_zero_it, region::mh_dual_element_value_zero_it,
    region::mh_element_value_format_zero_ja, region::mh_dual_element_value_zero_ja,
};

// --true-weapon-values needs the multipliers and the formats of at least one language; a release without them does not
// offer it.
// The FUComplete program (MHF_MHFU_PROGRAM_NAME mhfu-fuc): its own options replace true weapon values, and it rewrites
// the camera function the analog camera's call patches are in (they do not run there).
#if defined(MHF_MHFU_FUC) && MHF_MHFU_FUC
constexpr bool kFucomplete = true;
#else
constexpr bool kFucomplete = false;
#endif

constexpr bool kTrueWeaponValuesMapped =
    region::mh_attack_display_multipliers &&
    std::ranges::any_of(kElementValueFormatZeros,
                        [](const std::optional<std::uint32_t> &zero) { return zero.has_value(); });

// True when the byte at `zero` is the "0" that ends a "%<digit>d0" format: the loaded language's text is there, not
// another language's (and not one this function already cut short). The "0" must be the format's last character, so
// cutting it never drops more text.
bool is_value_format_zero(const psprecomp::GuestMemory &memory, std::uint32_t zero) {
    const std::uint8_t width = memory.load8(zero - 2u);
    return memory.load8(zero - 3u) == '%' && width >= '0' && width <= '9' && memory.load8(zero - 1u) == 'd' &&
           memory.load8(zero) == '0' && memory.load8(zero + 1u) == 0;
}

// Menus show a weapon's attack multiplied by its class's display multiplier / 100, and print a "0" after element
// values; damage uses the stored values. Writing 100 into every multiplier and ending the format text before that "0"
// shows the stored values. Done on every host poll: the game loads the format text after start-up and again when the
// player changes the language.
// The in-game menu turns it on and off while playing: off puts the game's multipliers back (read when the executable
// is loaded) and the "0" this cut, when the format text it cut is still there.
constexpr std::uint32_t kWeaponClasses = 11;
std::atomic<bool> g_true_weapon_values{false};
std::array<std::uint32_t, kWeaponClasses> g_display_multipliers{}; // the game's own, from the executable
bool g_true_values_shown = false;                                   // the multipliers hold 100 now

void read_display_multipliers([[maybe_unused]] Runtime &rt) {
    if constexpr (kTrueWeaponValuesMapped) {
        for (std::uint32_t i = 0; i < kWeaponClasses; ++i) {
            g_display_multipliers[i] = rt.memory().load32(*region::mh_attack_display_multipliers + i * 4u);
        }
    }
}

// True when the format at `zero` is one this cut short: "%<digit>d" then the end of the text where the "0" was.
bool is_cut_value_format(const psprecomp::GuestMemory &memory, std::uint32_t zero) {
    const std::uint8_t width = memory.load8(zero - 2u);
    return memory.load8(zero - 3u) == '%' && width >= '0' && width <= '9' && memory.load8(zero - 1u) == 'd' &&
           memory.load8(zero) == 0 && memory.load8(zero + 1u) == 0;
}

void show_true_weapon_values([[maybe_unused]] Runtime &rt) {
    if constexpr (kTrueWeaponValuesMapped) {
        const bool on = g_true_weapon_values.load();
        if (!on && !g_true_values_shown) return;
        for (std::uint32_t i = 0; i < kWeaponClasses; ++i) {
            rt.memory().store32(*region::mh_attack_display_multipliers + i * 4u, on ? 100u : g_display_multipliers[i]);
        }
        for (const auto &zero : kElementValueFormatZeros) {
            if (!zero) continue;
            if (on && is_value_format_zero(rt.memory(), *zero)) rt.memory().store8(*zero, std::uint8_t{0});
            if (!on && is_cut_value_format(rt.memory(), *zero)) rt.memory().store8(*zero, std::uint8_t{'0'});
        }
        g_true_values_shown = on;
    }
}

// Scripted input for unattended runs: "12.5:cross,15:start" presses each button for 150 ms at that time;
// "20:sup/3" holds the analog stick up (sup, sdown, sleft, sright) or a button for 3 seconds.
struct ScriptedPress {
    double at_seconds;
    std::uint32_t buttons;
    double duration = 0.15;
    int stick_x = 0, stick_y = 0;
};

std::vector<ScriptedPress> parse_input_script(const std::string &text) {
    static const std::pair<const char *, std::uint32_t> names[] = {
        {"cross", mhf::psp::buttons::kCross}, {"circle", mhf::psp::buttons::kCircle},
        {"square", mhf::psp::buttons::kSquare}, {"triangle", mhf::psp::buttons::kTriangle},
        {"start", mhf::psp::buttons::kStart}, {"select", mhf::psp::buttons::kSelect},
        {"up", mhf::psp::buttons::kUp}, {"down", mhf::psp::buttons::kDown},
        {"left", mhf::psp::buttons::kLeft}, {"right", mhf::psp::buttons::kRight},
        {"l", mhf::psp::buttons::kLTrigger}, {"r", mhf::psp::buttons::kRTrigger},
    };
    std::vector<ScriptedPress> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto comma = text.find(',', start);
        const std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const auto colon = item.find(':');
        if (colon != std::string::npos) {
            std::string name = item.substr(colon + 1);
            double duration = 0.15;
            if (const auto slash = name.find('/'); slash != std::string::npos) {
                duration = std::stod(name.substr(slash + 1));
                name.resize(slash);
            }
            const double at = std::stod(item.substr(0, colon));
            for (const auto &[n, bit] : names) {
                if (name == n) out.push_back({at, bit, duration});
            }
            if (name == "sup") out.push_back({at, 0, duration, 0, -127});
            if (name == "sdown") out.push_back({at, 0, duration, 0, 127});
            if (name == "sleft") out.push_back({at, 0, duration, -127, 0});
            if (name == "sright") out.push_back({at, 0, duration, 127, 0});
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// Hook on mh_overlay_prepare(image, size): the overlay image (header included) is already in guest memory and its
// static initializers are about to run, so its recompiled code must become the active one for its region now.
void on_overlay_prepare(Runtime &rt, Context &ctx) {
    const std::uint32_t image = ctx.gpr[4];
    const auto overlay_id = static_cast<std::int32_t>(rt.memory().load32(image + 4));
    const bool ok = rt.activate_overlay(overlay_id);
    if (g_log_overlays || !ok) {
        std::fprintf(stderr, "[mhfu] overlay %d %s at %08X\n", overlay_id, ok ? "activated" : "UNKNOWN", image);
    }
}

// --fast-supply-box. Taking an item out of the supply box sets a cooldown in the Player to 30 frames, and the box ignores
// every button until mh_supply_box_update has counted it down to 0. The offset is the same in the three releases.
constexpr std::uint32_t kPlayerSupplyBoxCooldown = 0x55E; // u8, frames
constexpr std::uint8_t kFastSupplyBoxFrames = 2;          // what is left of it: the box takes the next button 2 frames later

// Hook on mh_supply_box_update(?, Player*), before the game counts the cooldown down.
std::atomic<bool> g_fast_supply_box{false}; // the in-game menu turns it on and off while playing

void on_supply_box_update(Runtime &rt, Context &ctx) {
    if (!g_fast_supply_box.load()) return;
    const std::uint32_t cooldown = ctx.gpr[5] + kPlayerSupplyBoxCooldown;
    if (rt.memory().contains(cooldown) && rt.memory().load8(cooldown) > kFastSupplyBoxFrames) {
        rt.memory().store8(cooldown, kFastSupplyBoxFrames);
    }
}

// --smithy-browse. The smithy's lists ignore the d-pad while the R panel (the materials) or the Info panel is open; the
// region map's [patches] take those tests away, so the cursor moves under the panel. The R panel reads the item under
// the cursor every frame, but the Info panel is built once, when the player opens it: after a move it is built again.
constexpr const char *kSmithyBrowsePatches[] = {
    "mh_smithy_create_requires_blocks_up_down",  "mh_smithy_create_requires_blocks_paging",
    "mh_smithy_create_info_blocks_up_down",      "mh_smithy_improve_requires_blocks_up_down",
    "mh_smithy_improve_info_blocks_up_down",     "mh_smithy_decoration_requires_blocks_up_down",
    "mh_smithy_decoration_requires_blocks_paging",
};
constexpr bool kSmithyBrowseMapped = region::mh_smithy_create_info_setup && region::mh_smithy_improve_info_setup;
constexpr std::uint32_t kSmithyListInfoLevel = 0x22; // u8 in the list: 0 the list alone, 1 Info, 2 its skill page
constexpr std::uint32_t kSmithyListCursor = 0x3C;    // s16 in the list: the highlighted row
constexpr std::uint32_t kSmithyListInfoPage = 0x36;  // s16 in the list: the Info page shown (left/right)
constexpr std::uint32_t kSmithyListInfoPages = 0x38; // u8 in the list: how many pages Info has, set when it is built

struct SmithyInfoState {
    std::uint32_t list = 0;
    std::int16_t cursor = 0;
};
SmithyInfoState g_smithy_create_info, g_smithy_improve_info;

// The hooked smithy functions start functions in other overlays too: only the village overlay's are the smithy's.
bool village_overlay_at(const Runtime &rt, std::uint32_t address) {
    const mhf::psp::Program *overlay = rt.active_overlay_at(address);
    return overlay != nullptr && std::string_view(overlay->name).ends_with("_ovl_lobby_task");
}

// When Info is open and the cursor has moved since the panel was built, build it again with the game's own function
// `setup`, on the page the player was reading (the game builds it on the first page).
void refresh_smithy_info(Runtime &rt, Context &ctx, std::uint32_t list, std::uint32_t setup, SmithyInfoState &seen) {
    psprecomp::GuestMemory &memory = rt.memory();
    const auto cursor = static_cast<std::int16_t>(memory.load16(list + kSmithyListCursor));
    if (memory.load8(list + kSmithyListInfoLevel) == 0 || list != seen.list) {
        seen = {list, cursor};
        return;
    }
    if (cursor == seen.cursor) return;
    seen.cursor = cursor;
    const std::uint16_t page = memory.load16(list + kSmithyListInfoPage);
    Context call = ctx;
    rt.call(call, setup);
    if (page < memory.load8(list + kSmithyListInfoPages)) memory.store16(list + kSmithyListInfoPage, page);
}

// --smithy-layout (needs --smithy-browse). While Info is open: Info in the left column, where the weapon model, its box,
// the question box and the help bar were (all moved off screen; the model is drawn into an empty rectangle); the list
// stays visible on the right (a patch), with the materials panel under it without its item header (both panels are
// drawn: on_smithy_panel_draw); left/right pages the list (a patch) and the left stick turns Info's pages. Everything
// is the game's own tables, changed when Info opens and put back when it closes.
constexpr const char *kSmithyLayoutPatches[] = {"mh_smithy_list_hidden_with_info",
                                                "mh_smithy_create_info_takes_left_right"};
constexpr bool kSmithyLayoutMapped = kSmithyBrowseMapped && region::mh_smithy_panel_layout &&
                                     region::mh_smithy_side_layout && region::mh_smithy_question_box &&
                                     region::mh_smithy_skill_info_hint && region::mh_smithy_model_scissor &&
                                     region::mh_smithy_panel_draw_entry && region::mh_pad_input;
constexpr std::uint32_t kSmithyListFlags = 0x4C;        // u32 in the list: 0x20 while the materials panel is open
constexpr std::uint32_t kSmithyMaterialsOpen = 0x20u;
bool g_smithy_layout = false;

struct SmithyLayoutWrite {
    std::uint32_t address;
    std::uint16_t value;
};

std::vector<SmithyLayoutWrite> smithy_layout_writes() {
    if constexpr (!kSmithyLayoutMapped) {
        return {};
    } else {
        constexpr std::uint16_t kOffScreen = 600; // x past the 480-pixel screen
        const std::uint32_t panel = *region::mh_smithy_panel_layout, side = *region::mh_smithy_side_layout;
        const std::uint32_t question = *region::mh_smithy_question_box, hint = *region::mh_smithy_skill_info_hint;
        const std::uint32_t model = *region::mh_smithy_model_scissor;
        const auto pair = [](std::uint32_t table, std::uint32_t entry) { return table + entry * 4u; };
        return {
            {pair(panel, 10), 0},              // Info: the left column
            {pair(panel, 11) + 2, 180},        // materials: under the list's last row...
            {pair(panel, 12) + 2, 76},         // ...down to the list's bottom frame
            {pair(panel, 13), kOffScreen},     // materials: no item icon...
            {pair(panel, 14), kOffScreen},     // ...nor name
            {pair(panel, 15) + 2, 6},          // the first material row at the top
            {pair(panel, 17), 0},              // the list is not darkened under the materials
            {pair(panel, 17) + 2, 0},
            {pair(side, 0), kOffScreen},       // the weapon model's box
            {pair(side, 2), kOffScreen},       // the text in it (the Japanese release shows the model on SELECT)
            {pair(side, 3), kOffScreen},       // the help bar (zoom, turn the model)
            {question, kOffScreen},            // the question box...
            {question + 0xCu, kOffScreen},     // ...and its text
            {hint, 460},                       // the Skill Info hint ends under the list's right edge
            {hint + 2, 257},
            {model, 1},                        // the weapon model is drawn into an empty rectangle
            {model + 2, 1},
            {model + 4, 0},
            {model + 6, 0},
        };
    }
}

struct SmithyLayoutState {
    bool applied = false;
    std::vector<std::uint16_t> saved;
    bool stick_armed = true;
};
SmithyLayoutState g_smithy_layout_state;

void update_smithy_layout(Runtime &rt, bool info_open) {
    static const std::vector<SmithyLayoutWrite> writes = smithy_layout_writes();
    psprecomp::GuestMemory &memory = rt.memory();
    SmithyLayoutState &state = g_smithy_layout_state;
    if (info_open && !state.applied) {
        state.saved.clear();
        for (const auto &w : writes) {
            if (!memory.contains(w.address, 2)) return;
            state.saved.push_back(memory.load16(w.address));
        }
        for (const auto &w : writes) memory.store16(w.address, w.value);
        state.applied = true;
    } else if (!info_open && state.applied) {
        // Put the game's values back, unless the overlay was loaded again meanwhile (then they are fresh already).
        const bool ours = std::ranges::all_of(writes, [&](const SmithyLayoutWrite &w) {
            return memory.load16(w.address) == w.value;
        });
        if (ours) {
            for (std::size_t i = 0; i < writes.size(); ++i) memory.store16(writes[i].address, state.saved[i]);
        }
        state.applied = false;
    }
}

// The left stick turns Info's pages, one per push (the d-pad's left/right pages the list).
void page_smithy_info_with_stick(Runtime &rt, std::uint32_t list) {
    const int x = static_cast<int>(mhf::psp::controller_state(rt).lx) - 128;
    SmithyLayoutState &state = g_smithy_layout_state;
    if (std::abs(x) < 40) {
        state.stick_armed = true;
        return;
    }
    if (!state.stick_armed || std::abs(x) < 80) return;
    state.stick_armed = false;
    psprecomp::GuestMemory &memory = rt.memory();
    const unsigned pages = memory.load8(list + kSmithyListInfoPages);
    if (pages < 2) return;
    const unsigned page = memory.load16(list + kSmithyListInfoPage) % pages;
    memory.store16(list + kSmithyListInfoPage, static_cast<std::uint16_t>(x > 0 ? (page + 1) % pages : (page + pages - 1) % pages));
}

// On entry to a smithy list's per-frame function (List* in a0).
void on_smithy_list_update(Runtime &rt, Context &ctx, std::uint32_t setup, SmithyInfoState &seen) {
    if (!village_overlay_at(rt, setup)) return;
    const std::uint32_t list = ctx.gpr[4];
    if (!rt.memory().contains(list, kSmithyListFlags + 4)) return;
    refresh_smithy_info(rt, ctx, list, setup, seen);
    if (!g_smithy_layout) return;
    // Info only: its skill page (X again) keeps the game's own layout.
    const bool info_open = rt.memory().load8(list + kSmithyListInfoLevel) == 1;
    update_smithy_layout(rt, info_open);
    if (info_open) page_smithy_info_with_stick(rt, list);
}

void on_smithy_create_list_update(Runtime &rt, Context &ctx) {
    if constexpr (kSmithyBrowseMapped) on_smithy_list_update(rt, ctx, *region::mh_smithy_create_info_setup, g_smithy_create_info);
}

void on_smithy_improve_list_update(Runtime &rt, Context &ctx) {
    if constexpr (kSmithyBrowseMapped) on_smithy_list_update(rt, ctx, *region::mh_smithy_improve_info_setup, g_smithy_improve_info);
}

// Hook on mh_smithy_model_update(Model*): the left stick turns Info's pages while the layout is on, so the hidden weapon
// model must not turn with it (it would come back turned): the stick reads as centred for the model.
void on_smithy_model_update(Runtime &rt, Context &) {
    if constexpr (kSmithyLayoutMapped) {
        if (!g_smithy_layout_state.applied || !village_overlay_at(rt, *region::mh_smithy_panel_layout)) return;
        psprecomp::GuestMemory &memory = rt.memory();
        const std::uint32_t pad = memory.load32(*region::mh_pad_input);
        if (!memory.contains(pad, 5)) return;
        memory.store8(pad + 2, std::uint8_t{0});
        memory.store8(pad + 4, std::uint8_t{0});
    } else {
        (void)rt;
    }
}

// Hook on mh_smithy_panel_draw(List*): the game draws the materials panel or Info, never both. With the layout on and
// Info open, draw the materials first (the same function with the materials flag set for the call), then let it draw
// Info as usual.
void on_smithy_panel_draw(Runtime &rt, Context &ctx) {
    static bool drawing_materials = false;
    if constexpr (kSmithyLayoutMapped) {
        if (drawing_materials || !g_smithy_layout_state.applied) return;
        if (!village_overlay_at(rt, *region::mh_smithy_panel_layout)) return;
        const std::uint32_t list = ctx.gpr[4];
        psprecomp::GuestMemory &memory = rt.memory();
        if (!memory.contains(list, kSmithyListFlags + 4) || memory.load8(list + kSmithyListInfoLevel) != 1) return;
        const std::uint32_t flags = memory.load32(list + kSmithyListFlags);
        if ((flags & kSmithyMaterialsOpen) != 0) return;
        memory.store32(list + kSmithyListFlags, flags | kSmithyMaterialsOpen);
        drawing_materials = true;
        Context call = ctx;
        rt.call(call, *region::mh_smithy_panel_draw_entry);
        drawing_materials = false;
        memory.store32(list + kSmithyListFlags, flags);
    } else {
        (void)rt;
        (void)ctx;
    }
}

// --analog-camera. The game turns its camera with the d-pad by a fixed amount per frame and tilts it in five steps (a
// level 0-4 that picks one of five presets: where the eye sits and what it looks at). The region map's call patches
// run inside that d-pad code, so the right stick turns and tilts the camera by how far it is pushed exactly where the
// d-pad works. The offsets are the camera's (s4) and its tilt state's (s0) in that code. With --mouse-camera the mouse
// turns and tilts it the same way, its motion in a frame standing for a push (kMousePixelsPerPush).
constexpr int kRegV1 = 3, kRegA0 = 4, kRegA1 = 5, kRegS0 = 16, kRegS4 = 20;
constexpr std::uint32_t kCameraFov = 0x64;          // f32 in the camera: the field of view, set from the tilt level
constexpr std::uint32_t kCameraPresetTable = 0x6C;  // u32 in the camera: the presets (the area's own, or the default)
constexpr std::uint32_t kCameraPreset = 0x70;       // u32 in the camera: the preset the camera moves to
constexpr std::uint32_t kCameraButtons = 0x84;      // u16 in the camera: the buttons held, copied from the pad each frame
constexpr std::uint32_t kTiltLevel = 0x14;          // u8 in the tilt state: 0 (from above) to 4 (from below)
constexpr std::uint32_t kGameSysCameraSetting = 0x6ADE1; // u8 in GameSys: the camera setting, not 0 reverses the tilt
constexpr std::uint32_t kPresetSize = 0x20;         // 8 floats: the eye's offset (y, back) and the target's (y)
constexpr std::size_t kEyeY = 1, kEyeBack = 2, kTargetY = 5; // floats of a preset
constexpr float kFovLevel0 = std::bit_cast<float>(0x3F75BE0Bu); // the field of view the game sets at level 0 (55 degrees)
constexpr float kFovOther = std::bit_cast<float>(0x3F5F66F3u);  // and at levels 1-4 (50 degrees)
constexpr int kTiltLevels = 5;
// The stick tilts along a curve through the views of levels 0, 2, 3 and 4. Level 1 is a close-up: tilting through it
// would zoom in and out again, so the curve leaves it out (the d-pad still steps to it). Above the default view (level
// 2) the camera keeps that view's distance and field of view, so it only turns to look down: level 0's angle at the top.
constexpr std::array<int, 4> kTiltCurveLevels{0, 2, 3, 4};
constexpr float kStickDeadZone = 0.15f;
constexpr float kTiltPerFrame = 0.0384f; // radians at a full push (2.2 degrees): the whole range (about 62) in 28 frames
// The mouse's motion in one frame that counts as a full push of the stick, at 100% (pointer units), and the most a frame
// may count for (a fling past it turns no faster).
constexpr float kMousePixelsPerPush = 24.0f;
constexpr float kMouseMostPush = 8.0f;

// How far a stick axis is pushed past the dead zone, 0 to 1.
float stick_push(float axis) {
    const float push = (std::abs(axis) - kStickDeadZone) / (1.0f - kStickDeadZone);
    return std::clamp(push, 0.0f, 1.0f);
}

// The preset a tilt level picks: level 0 the table's sixth entry, levels 1-4 its second to fifth.
std::uint32_t tilt_preset(std::uint32_t table, int level) {
    return level == 0 ? table + 5u * kPresetSize : table + static_cast<std::uint32_t>(level) * kPresetSize;
}

// The eye's offset from what it looks at, and the angle it looks down at (negative: up).
struct CameraView {
    std::array<float, kPresetSize / 4> preset{};
    float fov{};
    [[nodiscard]] float rise() const { return preset[kEyeY] - preset[kTargetY]; }
    [[nodiscard]] float pitch() const { return std::atan2(rise(), preset[kEyeBack]); }
};

std::atomic<bool> g_analog_camera{false}; // --analog-camera, or the in-game menu while playing
std::atomic<float> g_turn_speed{1.0f};  // --analog-camera-turn-speed: a full push turns this many times as fast as the d-pad
std::atomic<float> g_tilt_speed{1.0f};  // --analog-camera-tilt-speed: scales kTiltPerFrame
std::atomic<bool> g_mouse_camera{false}; // --mouse-camera: the mouse turns and tilts the camera too (the analog camera's hooks)
std::atomic<float> g_mouse_speed{1.0f};  // --mouse-camera-speed: scales kMousePixelsPerPush's push
bool g_mouse_known = false;              // the mouse's motion has been read once
double g_mouse_last_x = 0.0, g_mouse_last_y = 0.0; // where its added-up motion stood at the last read
float g_mouse_dx = 0.0f, g_mouse_dy = 0.0f; // its motion this frame, 0 when the camera does not have the pad
float g_turn_rest = 0.0f;                // the part of a mouse turn too small for this frame, carried to the next
bool g_camera_has_pad = false; // the camera got the whole pad this frame (no menu or scene has it)
float g_turn_push = 0.0f;      // the stick's push for this frame's turn, 0 when the stick is not turning
bool g_tilt_known = false;
int g_tilt_level = 0;          // the level the tilt last saw or set: when the game's differs, the d-pad stepped
bool g_stick_tilted = false;   // the stick has tilted since that d-pad step: the curve holds the camera, not a level
float g_tilt_pitch = 0.0f;     // where on the curve, as the angle the camera looks down at
std::optional<std::uint32_t> g_tilt_preset_block; // where the curve's preset is written

// The camera has copied this frame's buttons from the pad (into s0, its button copy): a0 is 1 when it took them all.
// Otherwise a menu or a scene has the pad (the camera keeps only L, or nothing), and the stick leaves the camera alone
// too. The right stick presses the d-pad as well (bowgun aiming reads it), but the camera takes only the d-pad's own
// presses: a quick push would step its tilt level.
// Mouse aiming (--mouse-camera, while aiming a bowgun or a bow). Aiming does not move the camera: the d-pad turns the
// hunter's aim (the camera follows it) by 576 a frame and raises or lowers it by 5 a frame, from -100 to 100 (measured
// 2026-11-11, EU). While the hunter aims, the camera does not take the whole pad (a0 is 0 in the camera's hook) and
// the hunter's aim state is 5 (as it starts) or 10; then the mouse turns and raises the aim itself, a frame's
// motion of kMousePixelsPerPush counting as a frame of the d-pad.
constexpr std::uint32_t kGameSysPlayerSlot = 0x28;      // u8 in GameSys: the player slot this machine plays
constexpr std::uint32_t kPlayerManagerPlayers = 0x0C;   // Player*[6] in the PlayerManager, by player slot
constexpr std::uint32_t kPlayerAimYaw = 0x1F4;          // u16 in the Player: where the hunter faces and aims
constexpr std::uint32_t kPlayerAimYawCopy = 0x320;      // u16: its copy, kept equal while aiming
constexpr std::uint32_t kPlayerAimState = 0x11B0;       // u8: 5 as the aim starts, 10 while it holds
constexpr std::uint32_t kPlayerAimHeight = 0x67E;       // s8: the aim's height, -100 (down) to 100 (up)
constexpr float kAimTurnPerPush = 576.0f, kAimRaisePerPush = 5.0f;
constexpr int kAimHeightMost = 100;
float g_aim_turn_rest = 0.0f, g_aim_raise_rest = 0.0f;

// The Player of the hunter on this machine, 0 when there is none (horn_songs.cpp local_player).
std::uint32_t local_player(const psprecomp::GuestMemory &memory) {
    if constexpr (region::mh_game_sys.has_value() && region::mh_player_manager.has_value()) {
        const std::uint32_t sys = memory.load32(*region::mh_game_sys), manager = memory.load32(*region::mh_player_manager);
        if (!memory.contains(sys + kGameSysPlayerSlot) || !memory.contains(manager + kPlayerManagerPlayers, 24)) return 0;
        const std::uint32_t slot = memory.load8(sys + kGameSysPlayerSlot);
        if (slot >= 6) return 0;
        const std::uint32_t player = memory.load32(manager + kPlayerManagerPlayers + slot * 4);
        return memory.contains(player, kPlayerAimState + 1) ? player : 0;
    } else {
        (void)memory;
        return 0;
    }
}

void mouse_aim(Runtime &rt, float dx, float dy) {
    auto &memory = rt.memory();
    const std::uint32_t player = local_player(memory);
    const std::uint32_t state = player != 0 ? memory.load8(player + kPlayerAimState) : 0u;
    if (state != 5u && state != 10u) { // 5 as the aim starts, 10 while it holds
        g_aim_turn_rest = g_aim_raise_rest = 0.0f;
        return;
    }
    const float speed = g_mouse_speed.load() / kMousePixelsPerPush;
    // Left turns the aim up the angle, as the d-pad's Left; a turn and a raise too small for a whole step carry over.
    const float turn = std::clamp(-dx * speed, -kMouseMostPush, kMouseMostPush) * kAimTurnPerPush + g_aim_turn_rest;
    const long whole_turn = std::lround(turn);
    g_aim_turn_rest = turn - static_cast<float>(whole_turn);
    if (whole_turn != 0) {
        const auto yaw = static_cast<std::uint16_t>(memory.load16(player + kPlayerAimYaw) + static_cast<std::uint16_t>(whole_turn));
        memory.store16(player + kPlayerAimYaw, yaw);
        memory.store16(player + kPlayerAimYawCopy, yaw);
    }
    const float raise = std::clamp(-dy * speed, -kMouseMostPush, kMouseMostPush) * kAimRaisePerPush + g_aim_raise_rest;
    const long whole_raise = std::lround(raise);
    g_aim_raise_rest = raise - static_cast<float>(whole_raise);
    if (whole_raise != 0) {
        const int height = std::clamp(static_cast<int>(static_cast<std::int8_t>(memory.load8(player + kPlayerAimHeight))) +
                                          static_cast<int>(whole_raise), -kAimHeightMost, kAimHeightMost);
        memory.store8(player + kPlayerAimHeight, static_cast<std::uint8_t>(static_cast<std::int8_t>(height)));
    }
}

// --widescreen. The game's 3D camera keeps its aspect ratio (width over height) at camera + 8, which the projection reads
// for every picture. The two functions that set a camera up store the PSP's 480:272 there (built with lui/ori in v0);
// their call patches (the region map's mh_camera_aspect_made and mh_camera_aspect_reset) put the window's shape in v0
// instead, and the host poll writes it into the camera when the window or the setting changes, so it applies at once.
// The game has one such camera (EU 0x09998D30, set up again whenever a screen or a zone loads). The GL backend
// stretches the picture to the same shape (gl_backend.cpp).
constexpr std::uint32_t kCameraAspect = 8;             // float
constexpr std::uint32_t kPspAspectBits = 0x3FE1E1E2u;  // 480 / 272, as the game builds it
std::atomic<std::uint32_t> g_wanted_aspect{kPspAspectBits}; // from the frontend at every poll
std::uint32_t g_camera = 0;                             // the camera last set up
std::uint32_t g_camera_aspect = kPspAspectBits;         // what was put at its aspect

void set_camera_aspect(Context &ctx, std::uint32_t camera) {
    g_camera = camera;
    g_camera_aspect = g_wanted_aspect.load();
    ctx.gpr[2] = g_camera_aspect; // v0: what the next instruction stores at camera + 8
    static bool told = false;
    if (!told && g_camera_aspect != kPspAspectBits) {
        told = true;
        std::fprintf(stderr, "[mhfu] widescreen: the camera at %08X draws at %.3f:1\n", camera, std::bit_cast<float>(g_camera_aspect));
    }
}
void on_camera_aspect_made(Runtime &, Context &ctx) { set_camera_aspect(ctx, ctx.gpr[16]); }  // s0 is the camera
void on_camera_aspect_reset(Runtime &, Context &ctx) { set_camera_aspect(ctx, ctx.gpr[4]); }  // a0 is the camera

// At every host poll: the shape the frontend wants, written into the camera when it changed. Only while the camera
// still holds what was put there (anything else means the game set it itself, or reused that memory).
void keep_camera_aspect(Runtime &rt, float aspect) {
    const std::uint32_t wanted = aspect > 0.0f ? std::bit_cast<std::uint32_t>(aspect) : kPspAspectBits;
    g_wanted_aspect = wanted;
    if (g_camera == 0 || wanted == g_camera_aspect) return;
    auto &memory = rt.memory();
    if (!memory.contains(g_camera + kCameraAspect, 4) || memory.load32(g_camera + kCameraAspect) != g_camera_aspect) {
        g_camera = 0;
        return;
    }
    memory.store32(g_camera + kCameraAspect, wanted);
    g_camera_aspect = wanted;
}

void on_camera_buttons_copied(Runtime &rt, Context &ctx) {
    g_camera_has_pad = g_analog_camera.load() && ctx.gpr[kRegA0] == 1u; // off: the stick is the d-pad, as on the PSP
    // The mouse's motion since the last frame. It is read every frame, so motion while a menu or a scene had the pad is
    // dropped rather than turning the camera all at once afterwards.
    const auto &pad = mhf::psp::controller_state(rt);
    const double dx = g_mouse_known ? pad.mouse_x - g_mouse_last_x : 0.0, dy = g_mouse_known ? pad.mouse_y - g_mouse_last_y : 0.0;
    g_mouse_known = true;
    g_mouse_last_x = pad.mouse_x;
    g_mouse_last_y = pad.mouse_y;
    const bool mouse = g_camera_has_pad && g_mouse_camera.load();
    g_mouse_dx = mouse ? static_cast<float>(dx) : 0.0f;
    g_mouse_dy = mouse ? static_cast<float>(dy) : 0.0f;
    if (!mouse) g_turn_rest = 0.0f;
    // The camera does not have the pad: the hunter may be aiming, and then the mouse moves the aim.
    if (g_analog_camera.load() && g_mouse_camera.load() && ctx.gpr[kRegA0] != 1u) mouse_aim(rt, static_cast<float>(dx), static_cast<float>(dy));
    if (!g_camera_has_pad) return;
    constexpr std::uint32_t kDpad =
        mhf::psp::buttons::kUp | mhf::psp::buttons::kDown | mhf::psp::buttons::kLeft | mhf::psp::buttons::kRight;
    const std::uint32_t from_stick = kDpad & ~mhf::psp::controller_state(rt).dpad_buttons;
    const std::uint32_t copy = ctx.gpr[kRegS0] + kCameraButtons;
    if (!rt.memory().contains(copy, 4)) return;
    rt.memory().store16(copy, static_cast<std::uint16_t>(rt.memory().load16(copy) & ~from_stick));         // held
    rt.memory().store16(copy + 2, static_cast<std::uint16_t>(rt.memory().load16(copy + 2) & ~from_stick)); // pressed
}

// Before the camera tests the turn buttons (its button copy is in a1 as well): the stick, or else the mouse, holds Left
// or Right down.
void on_camera_turn_buttons_test(Runtime &rt, Context &ctx) {
    float x = mhf::psp::controller_state(rt).right_stick_x;
    g_turn_push = g_camera_has_pad ? stick_push(x) * g_turn_speed : 0.0f;
    if (g_turn_push == 0.0f && g_mouse_dx != 0.0f) {
        x = g_mouse_dx;
        g_turn_push = std::min(std::abs(g_mouse_dx) / kMousePixelsPerPush * g_mouse_speed, kMouseMostPush);
    } else {
        g_turn_rest = 0.0f;
    }
    if (g_turn_push == 0.0f) return;
    const std::uint32_t buttons = ctx.gpr[kRegS4] + kCameraButtons;
    if (!rt.memory().contains(buttons, 2)) return;
    constexpr std::uint32_t kTurn = mhf::psp::buttons::kLeft | mhf::psp::buttons::kRight;
    const std::uint32_t held = (rt.memory().load16(buttons) & ~kTurn) |
                               (x < 0.0f ? mhf::psp::buttons::kLeft : mhf::psp::buttons::kRight);
    rt.memory().store16(buttons, static_cast<std::uint16_t>(held));
    ctx.gpr[kRegA1] = held;
}

// The game has chosen this frame's turn (v1, its sign set by the camera setting): the push scales it. A mouse turn too
// small for a whole step carries its rest to the next frame, so slow motion still turns.
void on_camera_turn_amount_chosen(Runtime &, Context &ctx) {
    if (g_turn_push == 0.0f) return;
    const auto amount = static_cast<float>(static_cast<std::int32_t>(ctx.gpr[kRegV1]));
    float turn = amount * g_turn_push;
    if (g_mouse_dx != 0.0f) {
        if ((g_turn_rest < 0.0f) != (turn < 0.0f)) g_turn_rest = 0.0f; // the other way: start afresh
        turn += g_turn_rest;
        g_turn_rest = turn - static_cast<float>(std::lround(turn));
    }
    ctx.gpr[kRegV1] = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(turn)));
    g_turn_push = 0.0f;
}

using TiltCurve = std::array<CameraView, kTiltCurveLevels.size()>;

// The view on the tilt curve at `pitch` (between the first and last views' pitches): on the segment between two of
// the views, the point the camera looks down from at exactly that angle; above the default view, moved out to its
// distance.
CameraView tilt_curve_view(const TiltCurve &curve, float pitch) {
    std::size_t segment = 0;
    while (segment + 2 < curve.size() && pitch < curve[segment + 1].pitch()) ++segment;
    const CameraView &a = curve[segment], &b = curve[segment + 1];
    // rise(s) / back(s) = tan(pitch), both linear in s.
    const float slope = std::tan(pitch);
    const float back_a = a.preset[kEyeBack], back_b = b.preset[kEyeBack];
    const float denominator = (b.rise() - a.rise()) - slope * (back_b - back_a);
    const float s = denominator != 0.0f ? std::clamp((slope * back_a - a.rise()) / denominator, 0.0f, 1.0f) : 0.0f;
    CameraView view;
    for (std::size_t i = 0; i < view.preset.size(); ++i) view.preset[i] = a.preset[i] + (b.preset[i] - a.preset[i]) * s;
    view.fov = a.fov + (b.fov - a.fov) * s;
    if (pitch > curve[1].pitch()) { // above the default view: its distance and field of view
        const CameraView &level2 = curve[1];
        const float scale = std::hypot(level2.rise(), level2.preset[kEyeBack]) /
                            std::hypot(view.rise(), view.preset[kEyeBack]);
        const float rise = view.rise() * scale;
        view.preset[kEyeBack] *= scale;
        view.preset[kEyeY] = view.preset[kTargetY] + rise;
        view.fov = level2.fov;
    }
    return view;
}

// The camera has picked its preset and field of view from the tilt level. The stick moves the camera along the tilt
// curve, and the level follows to the nearest one. The d-pad still steps the level, and the camera goes to that
// level's own view until the stick tilts again.
void on_camera_tilt_preset_chosen(Runtime &rt, Context &ctx) {
    if (!g_analog_camera.load()) { // off (the in-game menu): the game's own views; on again, the tilt starts afresh
        g_tilt_known = false;
        return;
    }
    auto &memory = rt.memory();
    const std::uint32_t camera = ctx.gpr[kRegS4];
    const std::uint32_t level_at = ctx.gpr[kRegS0] + kTiltLevel;
    if (!memory.contains(camera + kCameraFov, kCameraPreset + 4u - kCameraFov) || !memory.contains(level_at)) return;
    const std::uint32_t table = memory.load32(camera + kCameraPresetTable);
    const int level = memory.load8(level_at);
    // A camera with a fixed preset of its own is not tilted.
    if (level >= kTiltLevels || memory.load32(camera + kCameraPreset) != tilt_preset(table, level)) return;

    std::array<CameraView, kTiltLevels> levels;
    for (int l = 0; l < kTiltLevels; ++l) {
        const std::uint32_t at = tilt_preset(table, l);
        if (!memory.contains(at, kPresetSize)) return;
        for (std::size_t i = 0; i < levels[l].preset.size(); ++i)
            levels[l].preset[i] = std::bit_cast<float>(memory.load32(at + static_cast<std::uint32_t>(i) * 4u));
        levels[l].fov = l == 0 ? kFovLevel0 : kFovOther;
    }
    TiltCurve curve;
    for (std::size_t i = 0; i < curve.size(); ++i) {
        curve[i] = levels[kTiltCurveLevels[i]];
        // The curve needs each view to look down less than the one before (the game's do); otherwise leave the levels.
        if (curve[i].preset[kEyeBack] <= 0.0f || (i > 0 && curve[i].pitch() >= curve[i - 1].pitch())) return;
    }

    if (!g_tilt_known || level != g_tilt_level) { // the first frame, or the d-pad stepped
        g_tilt_known = true;
        g_tilt_level = level;
        g_stick_tilted = false;
        g_tilt_pitch = levels[level].pitch();
    }
    float y = mhf::psp::controller_state(rt).right_stick_y;
    float push = g_camera_has_pad ? stick_push(y) * g_tilt_speed : 0.0f;
    if (push == 0.0f && g_mouse_dy != 0.0f) { // the mouse, when the stick is not tilting: up looks up, as the stick
        y = g_mouse_dy;
        push = std::min(std::abs(g_mouse_dy) / kMousePixelsPerPush * g_mouse_speed, kMouseMostPush);
    }
    if (push > 0.0f) {
        // Up raises the level (the camera looks from lower down), as the d-pad does, unless the setting reverses it.
        bool reversed = false;
        if constexpr (region::mh_game_sys.has_value()) {
            const std::uint32_t setting = memory.load32(*region::mh_game_sys) + kGameSysCameraSetting;
            reversed = memory.contains(setting) && memory.load8(setting) != 0;
        }
        const float raise = (y < 0.0f) != reversed ? push : -push;
        g_tilt_pitch = std::clamp(g_tilt_pitch - raise * kTiltPerFrame, curve.back().pitch(), curve.front().pitch());
        g_stick_tilted = true;
        int nearest = 0;
        for (int l = 1; l < kTiltLevels; ++l) {
            if (std::abs(levels[l].pitch() - g_tilt_pitch) < std::abs(levels[nearest].pitch() - g_tilt_pitch)) nearest = l;
        }
        g_tilt_level = nearest;
        memory.store8(level_at, static_cast<std::uint8_t>(nearest));
    }
    if (!g_stick_tilted) return; // the game's own view of the level

    if (!g_tilt_preset_block) g_tilt_preset_block = rt.allocate_kernel_block("analog camera preset", kPresetSize);
    if (!g_tilt_preset_block) return;
    const float pitch = std::clamp(g_tilt_pitch, curve.back().pitch(), curve.front().pitch()); // areas differ
    const CameraView view = tilt_curve_view(curve, pitch);
    for (std::size_t i = 0; i < view.preset.size(); ++i) {
        memory.store32(*g_tilt_preset_block + static_cast<std::uint32_t>(i) * 4u,
                       std::bit_cast<std::uint32_t>(view.preset[i]));
    }
    memory.store32(camera + kCameraPreset, *g_tilt_preset_block);
    memory.store32(camera + kCameraFov, std::bit_cast<std::uint32_t>(view.fov)); // the game's wider one, if set, follows
}

// The disc's product code ("ULES01213") from its PARAM.SFO, which ad hoc servers use to group the players of one game.
std::string disc_product_code(const std::filesystem::path &disc_image) {
    try {
        mhf::psp::IsoImage image(disc_image);
        if (const auto entry = image.find("/PSP_GAME/PARAM.SFO")) {
            if (const auto sfo = mhf::psp::ParamSfo::parse(image.read_file(*entry, 0, entry->size))) return sfo->string("DISC_ID");
        }
    } catch (const std::exception &) {
    }
    return {};
}

// "AA:BB:CC:DD:EE:FF" (or with '-') as bytes. False for anything else.
bool parse_mac(const std::string &text, std::array<std::uint8_t, 6> &mac) {
    unsigned v[6];
    char sep[5];
    if (std::sscanf(text.c_str(), "%2x%c%2x%c%2x%c%2x%c%2x%c%2x", &v[0], &sep[0], &v[1], &sep[1], &v[2], &sep[2], &v[3], &sep[3], &v[4],
                    &sep[4], &v[5]) != 11) {
        return false;
    }
    for (const char c : sep) {
        if (c != ':' && c != '-') return false;
    }
    for (int i = 0; i < 6; ++i) mac[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v[i]);
    return true;
}

// "MHForever - <the disc's own title>": the window names the game that is running. The title comes from the disc image's
// PARAM.SFO (upper-case titles are shown in title case); without a readable image the window is just "MHForever".
std::string window_title(const std::filesystem::path &disc_image) {
    std::string title;
    try {
        if (!disc_image.empty()) {
            mhf::psp::IsoImage image(disc_image);
            if (const auto entry = image.find("/PSP_GAME/PARAM.SFO")) {
                if (const auto sfo = mhf::psp::ParamSfo::parse(image.read_file(*entry, 0, entry->size))) {
                    title = sfo->string("TITLE");
                }
            }
        }
    } catch (const std::exception &) {
    }
    // The disc's title carries a trademark sign (UTF-8 "\xE2\x84\xA2" or "\xC2\xAE"): not wanted in a window title.
    for (const char *mark : {"\xE2\x84\xA2", "\xC2\xAE"}) {
        for (std::size_t at = title.find(mark); at != std::string::npos; at = title.find(mark)) title.erase(at, std::strlen(mark));
    }
    while (!title.empty() && (title.back() == ' ' || title.back() == '\n')) title.pop_back();
    // A patched disc may end its title with its own version ("... COMPLETE v1.4.0"): dropped.
    if (const auto space = title.rfind(' '); space != std::string::npos && space + 2 < title.size() &&
        (title[space + 1] == 'v' || title[space + 1] == 'V') && title[space + 2] >= '0' && title[space + 2] <= '9') {
        title.erase(space);
    }
    // The taskbar cuts long window titles: a title longer than the retail "MONSTER HUNTER FREEDOM UNITE" (FUComplete's
    // "MONSTER HUNTER FREEDOM UNITE COMPLETE") drops its "MONSTER HUNTER " prefix, as the window already says MHForever.
    constexpr std::string_view kPrefix = "MONSTER HUNTER ";
    if (title.size() > 30 && title.size() > kPrefix.size()) {
        bool prefix = true;
        for (std::size_t i = 0; i < kPrefix.size(); ++i) {
            const char c = title[i] >= 'a' && title[i] <= 'z' ? static_cast<char>(title[i] - 'a' + 'A') : title[i];
            prefix = prefix && c == kPrefix[i];
        }
        if (prefix) title.erase(0, kPrefix.size());
    }
    if (title.empty()) return "MHForever";
    bool all_upper = true;
    for (const char c : title) all_upper = all_upper && !(c >= 'a' && c <= 'z');
    if (all_upper) {
        bool word_start = true;
        for (char &c : title) {
            const bool letter = c >= 'A' && c <= 'Z';
            if (letter && !word_start) c = static_cast<char>(c - 'A' + 'a');
            word_start = c == ' ' || c == '-';
        }
    }
    return "MHForever - " + title;
}

} // namespace

int main(int argc, char **argv) {
    mhf::psp::RuntimeOptions options;
    std::string exe;
    std::filesystem::path dump_folder;
    unsigned dump_every = 30;
    double dump_from = 0.0; // seconds of run time before frames are dumped
    double seconds = 0;
    bool window = false;
    bool auto_dialogs = false;
    bool software = false; // --software: draw with the CPU renderer even in a window
    int render_scale = 4;  // --scale N: internal resolution = N x 480x272 (GPU renderer)
    bool fullscreen = false;
    bool smooth_textures = false;
    int target_fps = 30; // --fps N: 30 (the game's own rate) or a multiple of it up to 180
    int hud_size = 100; // --hud-size N: the quest HUD's size in percent (enhancement)
    bool widescreen = false; // --widescreen: the 3D fills a window wider than the PSP's screen (enhancement)
    // The projected shadows (enhancement): --projected-shadows turns them on, --no-whole-map-shadows leaves only the hunter and
    // monsters casting, --shadow-quality 0..2 (low by default), --shadow-darkness 0..100 (50: as each zone was tuned).
    bool projected_shadows = false, whole_map_shadows = true;
    int shadow_quality = 0, shadow_darkness = 50;
    bool true_weapon_values = false; // --true-weapon-values: menus show attack and element without display multipliers
    bool fast_supply_box = false; // --fast-supply-box: the supply box takes the next button soon after a take-out
    bool smithy_browse = false; // --smithy-browse: the smithy's lists scroll with the R or Info panel open
    bool smithy_layout = false; // --smithy-layout: with Info open, Info on the left and the list with materials on the right
    bool analog_camera = false; // --analog-camera: the right stick turns and tilts the camera by how far it is pushed
    bool horn_songs = false; // --horn-songs: the hunting horn's songs over the quest HUD (enhancement)
    std::string horn_songs_place = "next-to-sheet"; // --horn-songs-place: where (overlay/song_panel.hpp, parse_song_spot)
    mhf::host::KeyBindings keyboard; // --keymap: the launcher's keyboard bindings
    mhf::host::PadBindings controller; // --padmap: the launcher's controller bindings
    std::string controls_file;         // --controls-file: where the in-game menu writes the bindings the player changes
    std::string texture_pack;          // --texture-pack: a texture pack's folder (MHF_TEXTURE_PACK too; the argument wins)
    bool pack_mipmaps = false;         // --pack-mipmaps: mip levels made for the pack's images that ship none
    int anisotropy = 16;               // --anisotropy N: the pack's anisotropic filtering, 1 (off) to 16
    std::string friends_file, friend_actions_file; // --friends-file, --friend-actions-file: friends in the Online tab
    std::string adhoc_nick;            // --adhoc-nick: the name other players see online (default: --name)
    std::vector<ScriptedPress> script;
    bool language_pick = false; // --language was given: the game screen that confirms it is answered by one press
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--iso" && i + 1 < argc) options.disc_image = argv[++i];
        else if (a == "--exe" && i + 1 < argc) exe = argv[++i];
        else if (a == "--memstick" && i + 1 < argc) options.memstick_root = argv[++i];
        else if (a == "--trace") options.trace_imports = true;
        else if (a == "--quiet-overlays") g_log_overlays = false;
        else if (a == "--dump-frames" && i + 1 < argc) dump_folder = argv[++i];
        else if (a == "--dump-every" && i + 1 < argc) dump_every = static_cast<unsigned>(std::stoul(argv[++i]));
        else if (a == "--dump-from" && i + 1 < argc) dump_from = std::stod(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::stod(argv[++i]);
        else if (a == "--window") window = true;
        else if (a == "--software") software = true;
        else if (a == "--scale" && i + 1 < argc) render_scale = std::clamp(std::stoi(argv[++i]), 1, 10);
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--smooth-textures") smooth_textures = true; // enhancement, off by default
        else if (a == "--fps" && i + 1 < argc) target_fps = std::clamp(std::stoi(argv[++i]), 30, 180); // enhancement
        else if (a == "--hud-size" && i + 1 < argc) hud_size = std::clamp(std::stoi(argv[++i]), 50, 100); // enhancement
        else if (a == "--widescreen") widescreen = true; // enhancement, off by default
        else if (a == "--projected-shadows") projected_shadows = true; // enhancement, off by default
        else if (a == "--no-whole-map-shadows") whole_map_shadows = false;
        else if (a == "--shadow-quality" && i + 1 < argc) shadow_quality = std::clamp(std::stoi(argv[++i]), 0, 2);
        else if (a == "--shadow-darkness" && i + 1 < argc) shadow_darkness = std::clamp(std::stoi(argv[++i]), 0, 100);
        else if (a == "--true-weapon-values") true_weapon_values = true; // enhancement, off by default
        else if (a == "--fast-supply-box") fast_supply_box = true; // enhancement, off by default
        else if (a == "--smithy-browse") smithy_browse = true; // enhancement, off by default
        else if (a == "--smithy-layout") smithy_layout = true; // enhancement, off by default; needs --smithy-browse
        else if (a == "--analog-camera") analog_camera = true; // enhancement, off by default
        else if (a == "--analog-camera-turn-speed" && i + 1 < argc)
            g_turn_speed = static_cast<float>(std::clamp(std::stoi(argv[++i]), 50, 200)) / 100.0f; // in percent
        else if (a == "--analog-camera-tilt-speed" && i + 1 < argc)
            g_tilt_speed = static_cast<float>(std::clamp(std::stoi(argv[++i]), 50, 200)) / 100.0f; // in percent
        else if (a == "--mouse-camera") g_mouse_camera = true; // enhancement, off by default; works with --analog-camera
        else if (a == "--mouse-camera-speed" && i + 1 < argc)
            g_mouse_speed = static_cast<float>(std::clamp(std::stoi(argv[++i]), 25, 300)) / 100.0f; // in percent
        else if (a == "--horn-songs") horn_songs = true; // enhancement
        else if (a == "--horn-songs-place" && i + 1 < argc) horn_songs_place = argv[++i];
        else if (a == "--music-volume" && i + 1 < argc) options.music_volume = std::clamp(std::stoi(argv[++i]), 0, 100);
        else if (a == "--sfx-volume" && i + 1 < argc) options.sfx_volume = std::clamp(std::stoi(argv[++i]), 0, 100);
        else if (a == "--language" && i + 1 < argc) {
            // The PSP's system language, which the game reads with sceUtilityGetSystemParamInt(8). English by default.
            const std::string_view code = argv[++i];
            const auto found = std::find_if(std::begin(kLanguages), std::end(kLanguages),
                                            [code](const auto &entry) { return entry.first == code; });
            if (found == std::end(kLanguages)) {
                std::fprintf(stderr, "error: unknown --language '%.*s' (en, fr, de, es, it, jp)\n",
                             static_cast<int>(code.size()), code.data());
                return 2;
            }
            options.system_language = found->second;
            language_pick = true; // the screen that answers after the highlight is satisfied by the game itself
        }
        else if (a == "--keymap" && i + 1 < argc) keyboard = mhf::host::parse_keymap(argv[++i]);
        else if (a == "--padmap" && i + 1 < argc) controller = mhf::host::parse_padmap(argv[++i]);
        else if (a == "--controls-file" && i + 1 < argc) controls_file = argv[++i];
        else if (a == "--texture-pack" && i + 1 < argc) texture_pack = argv[++i];
        else if (a == "--pack-mipmaps") pack_mipmaps = true;
        else if (a == "--anisotropy" && i + 1 < argc) anisotropy = std::clamp(std::stoi(argv[++i]), 1, 16);
        else if (a == "--friends-file" && i + 1 < argc) friends_file = argv[++i];
        else if (a == "--friend-actions-file" && i + 1 < argc) friend_actions_file = argv[++i];
        else if (a == "--adhoc-server" && i + 1 < argc) options.adhoc_server = argv[++i]; // online play: an ad hoc server (PPSSPP's)
        else if (a == "--adhoc-nick" && i + 1 < argc) adhoc_nick = argv[++i];
        else if (a == "--adhoc-room" && i + 1 < argc) options.adhoc_room = argv[++i]; // a private room's code
        else if (a == "--adhoc-mac" && i + 1 < argc) {
            if (!parse_mac(argv[++i], options.adhoc_mac)) std::fprintf(stderr, "[mhfu] --adhoc-mac: not a MAC address, ignored\n");
        }
        else if (a == "--input" && i + 1 < argc) script = parse_input_script(argv[++i]);
        else if (a == "--name" && i + 1 < argc) options.osk_default_text = argv[++i]; // text typed into keyboards
        else if (a == "--auto-dialogs") auto_dialogs = true; // system dialogs answer at once even in a window
        else {
            std::fprintf(stderr, "usage: %s --iso <image.iso> --exe <decrypted executable> [--trace] [--memstick <dir>]\n",
                         argv[0]);
            return 2;
        }
    }
    if (options.disc_image.empty() || exe.empty()) {
        std::fprintf(stderr, "error: --iso and --exe are required\n");
        return 2;
    }
    if constexpr (!kTrueWeaponValuesMapped) {
        if (true_weapon_values) {
            std::fprintf(stderr, "[mhfu] --true-weapon-values: not mapped for this release, ignored\n");
            true_weapon_values = false;
        }
    }
    // In a window the player answers the system dialogs (messages, keyboard, save/load) on screen.
    options.interactive_dialogs = window && !auto_dialogs;
    if (texture_pack.empty()) {
        if (const char *pack = std::getenv("MHF_TEXTURE_PACK")) texture_pack = pack;
    }
    if (options.adhoc_server.empty()) {
        if (const char *server = std::getenv("MHF_ADHOC_SERVER")) options.adhoc_server = server; // until the launcher sets it
    }
    options.adhoc_nickname = adhoc_nick.empty() ? options.osk_default_text : adhoc_nick;
    if (!options.adhoc_server.empty()) options.adhoc_product_code = disc_product_code(options.disc_image);
    // The halls' names start with these (docs/14-rooms-and-friends.md section 1): Freedom Unite's two modes, and the
    // Japanese release's "集会所" in Shift-JIS. Any other prefix the game uses is learnt when it joins a hall.
    options.adhoc_hall_prefixes = {"MHP2Q0", "MHP2A0", "\x8F\x57\x89\xEF\x8F\x8A"};
    try {
        Runtime runtime{options};
        runtime.load_elf(exe);
        read_display_multipliers(runtime);
        g_true_weapon_values = true_weapon_values;
        const auto programs = mhf::psp::generated::mhfu_programs();
        for (std::size_t i = 1; i < programs.size(); ++i) {
            runtime.add_overlay(*programs[i]);
        }
        runtime.register_hook("mh_overlay_prepare", &on_overlay_prepare);
        // Hooks that read a switch every time, so the in-game menu can turn their enhancement on and off.
        g_fast_supply_box = fast_supply_box;
        runtime.register_hook("mh_supply_box_update", &on_supply_box_update);
        if (smithy_layout) smithy_browse = true; // the layout scrolls the list with Info open
        if (smithy_browse && kSmithyBrowseMapped) {
            for (const char *patch : kSmithyBrowsePatches) runtime.enable_patch(patch);
            runtime.register_hook("mh_smithy_create_list_update", &on_smithy_create_list_update);
            runtime.register_hook("mh_smithy_improve_list_update", &on_smithy_improve_list_update);
            if (smithy_layout && kSmithyLayoutMapped) {
                for (const char *patch : kSmithyLayoutPatches) runtime.enable_patch(patch);
                runtime.register_hook("mh_smithy_panel_draw", &on_smithy_panel_draw);
                runtime.register_hook("mh_smithy_model_update", &on_smithy_model_update);
                g_smithy_layout = true;
            } else if (smithy_layout) {
                std::fprintf(stderr, "[mhfu] --smithy-layout: not mapped for this release, ignored\n");
            }
        } else if (smithy_browse) {
            std::fprintf(stderr, "[mhfu] --smithy-browse: not mapped for this release, ignored\n");
        }
        g_analog_camera = analog_camera;
        { // the region map's call patches (FUComplete's camera function runs in the interpreter, which runs them too)
            runtime.register_hook("mh_camera_aspect_made", &on_camera_aspect_made); // widescreen
            runtime.register_hook("mh_camera_aspect_reset", &on_camera_aspect_reset);
            runtime.register_hook("mh_camera_buttons_copied", &on_camera_buttons_copied);
            runtime.register_hook("mh_camera_turn_buttons_test", &on_camera_turn_buttons_test);
            runtime.register_hook("mh_camera_turn_amount_chosen", &on_camera_turn_amount_chosen);
            runtime.register_hook("mh_camera_tilt_preset_chosen", &on_camera_tilt_preset_chosen);
        }
        std::function<void()> extra;
        if (!dump_folder.empty() || seconds > 0 || !script.empty() || language_pick) {
            if (!dump_folder.empty()) std::filesystem::create_directories(dump_folder);
            auto *rt = &runtime;
            std::uint64_t last_vblank = 0;
            unsigned frame = 0;
            // The boot's language screen is the first screen that waits for input. When the player picked a language
            // with --language (the launcher shortcut already selects it), the game highlights that language, so one
            // confirm press satisfies the screen and the boot goes on without a language step.
            //
            // Sending the press blind (a fixed time after the boot) races the player's first seconds: on a slow boot
            // it could land before the screen, on a fast one long after. Instead the press waits for the screen: the
            // boot's scene counter (a data symbol of the region, see kLanguageScreenStageAddress) steps through its
            // start-up screens and holds a known value while the language screen waits, verified against dumps of the
            // whole guest RAM paired with the dumped display.
            // The language is preselected by the runtime's answer either way; the press only happens where the
            // region's symbol map names that counter.
            // The marker fires while the screen is still fading in and ignoring input, so the cross is pulsed (press
            // and release, each ~150 ms) until the screen answers and the stage moves on. A fresh press is what the
            // screen reads; a cross merely held since the fade-in never counts. The sequence is capped and sent once.
            constexpr std::uint64_t kLanguagePressMaxUs = 6000000;
            constexpr std::uint64_t kLanguagePulseUs = 9 * mhf::psp::kVblankPeriodUs;
            bool language_screen_handled = false;
            std::uint64_t language_press_deadline = 0;
            extra = [=]() mutable {
                const std::uint64_t now = rt->now_us();
                if (seconds > 0 && static_cast<double>(now) > seconds * 1e6) {
                    std::fprintf(stderr, "[mhfu] time limit reached\n");
                    rt->dump_threads();
                    rt->exit_program_from_host(0);
                }
                const std::uint32_t stage =
                    kLanguageScreenStageAddress ? rt->memory().load32(*kLanguageScreenStageAddress) : 0u;
                const bool language_screen_up =
                    kLanguageScreenStageAddress.has_value() && stage == kLanguageScreenStageValue;
                if (language_pick && !language_screen_handled && language_screen_up) {
                    language_screen_handled = true; // one press sequence: the screen answers and the boot continues
                    language_press_deadline = now + kLanguagePressMaxUs;
                }
                const bool answering_language_screen =
                    language_pick && language_screen_handled && language_screen_up && now < language_press_deadline;
                const bool press_language = answering_language_screen && (now / kLanguagePulseUs) % 2 == 0;
                // The pad state is refreshed on every poll while answering, including the released half: leaving it
                // untouched would keep the cross latched and the screen would never see a fresh press. Only while
                // answering, though: the frontend sets the player's own input on the same poll before this runs, so
                // writing here afterwards would throw it away and leave the controls dead.
                if (!script.empty() || answering_language_screen) {
                    mhf::psp::ControllerState pad;
                    for (const auto &p : script) {
                        const double t = static_cast<double>(now) / 1e6;
                        if (t < p.at_seconds || t >= p.at_seconds + p.duration) continue;
                        pad.buttons |= p.buttons;
                        if (p.stick_x != 0) pad.lx = static_cast<std::uint8_t>(128 + p.stick_x);
                        if (p.stick_y != 0) pad.ly = static_cast<std::uint8_t>(128 + p.stick_y);
                    }
                    if (press_language) pad.buttons |= mhf::psp::buttons::kCross;
                    pad.dpad_buttons = pad.buttons & (mhf::psp::buttons::kUp | mhf::psp::buttons::kDown |
                                                      mhf::psp::buttons::kLeft | mhf::psp::buttons::kRight);
                    mhf::psp::set_controller_state(*rt, pad);
                }
                // One dump per `dump_every` vblanks, even when the host is not polled on that exact vblank.
                const std::uint64_t slot = now / mhf::psp::kVblankPeriodUs / dump_every;
                if (dump_folder.empty() || slot == last_vblank || static_cast<double>(now) < dump_from * 1e6) return;
                last_vblank = slot;
                char name[32];
                std::snprintf(name, sizeof name, "frame_%06u.png", ++frame);
                mhf::psp::dump_display(*rt, dump_folder / name);
            };
        }
        // In a window the in-game menu can turn true weapon values on later, so they are looked after on every poll.
        if (true_weapon_values || (window && kTrueWeaponValuesMapped && !kFucomplete)) {
            // Headless runs then always have a host poll, so the kernel no longer reports a deadlock (kernel.cpp).
            extra = [rt = &runtime, previous = std::move(extra)]() {
                show_true_weapon_values(*rt);
                if (previous) previous();
            };
        }
#if defined(MHF_HAS_FRONTEND)
        mhf::psp::Frontend frontend;
        if (window && frontend.open(window_title(options.disc_image).c_str(), false, 2, !software, render_scale, fullscreen, smooth_textures, target_fps, keyboard, controller)) {
            frontend.set_controls_file(controls_file);
            frontend.set_friends_files(friends_file, friend_actions_file);
            if (!texture_pack.empty()) frontend.set_texture_pack(texture_pack, disc_product_code(options.disc_image), pack_mipmaps, anisotropy);
            frontend.set_shadow_textures({std::begin(kShadowAirTextures), std::end(kShadowAirTextures)},
                                         {std::begin(kShadowSolidTextures), std::end(kShadowSolidTextures)},
                                         {}); // caves go by the zones below
            frontend.set_shadow_cave_zones({std::begin(kShadowCaveZones), std::end(kShadowCaveZones)});
            frontend.set_shadow_haze_textures({std::begin(kShadowHazeTextures), std::end(kShadowHazeTextures)});
            frontend.set_shadow_zone_suns(shadow_zone_suns());
            frontend.set_shadow_zones({std::begin(kShadowZoneNames), std::end(kShadowZoneNames)}, shadow_zone_textures());
            frontend.set_hud(hud_size);
            frontend.set_widescreen(widescreen);
            const char *shadow_env = std::getenv("MHF_SHADOW"); // MHF_SHADOW=1 turns them on as --projected-shadows does
            frontend.set_shadows(projected_shadows || (shadow_env != nullptr && shadow_env[0] != '0'), whole_map_shadows, shadow_quality,
                                 shadow_darkness);
            // The horn's songs over the quest HUD, read from the game a few times a second (horn_songs.hpp). The
            // in-game menu turns them on and off, so they are read even while off.
            frontend.offer_songs(horn_songs, mhf::overlay::parse_song_spot(horn_songs_place).value_or(mhf::overlay::SongSpot{}));
            extra = [&frontend, rt = &runtime, songs = mhf::mhfu::HornSongs{}, read_at = std::uint64_t{0},
                     previous = std::move(extra)]() mutable {
                if (const std::uint64_t now = rt->now_us(); now - read_at >= 50'000) {
                    read_at = now;
                    frontend.set_song_panel(songs.read(*rt));
                }
                if (previous) previous();
            };
            // The enhancements the in-game menu changes while playing; the launcher keeps what is chosen there.
            const auto percent = [](float speed) { return static_cast<int>(std::lround(speed * 100.0f)); };
            if (kTrueWeaponValuesMapped && !kFucomplete) {
                frontend.add_game_option({"trueWeaponValues", "Show true weapon values",
                                          "Displays attack and element without the game's display multipliers.", false,
                                          g_true_weapon_values ? 1 : 0, 0, 1, 1, "",
                                          [](int v) { g_true_weapon_values = v != 0; }});
            }
            frontend.add_game_option({"fastSupplyBox", "Fast supply box",
                                      "Shortens the delay between items taken from the supply box.",
                                      false, g_fast_supply_box ? 1 : 0, 0, 1, 1, "",
                                      [](int v) { g_fast_supply_box = v != 0; }});
            {
                frontend.add_game_option({"analogCamera", "Analog camera",
                                          "The right stick turns and tilts the camera in proportion to how far it is pushed.",
                                          false, g_analog_camera ? 1 : 0, 0, 1, 1, "",
                                          [](int v) { g_analog_camera = v != 0; }});
                frontend.add_game_option({"analogCameraTurnSpeed", "Camera turn speed", "100% matches the D-pad.", true,
                                          percent(g_turn_speed), 50, 200, 10, "analogCamera",
                                          [](int v) { g_turn_speed = static_cast<float>(v) / 100.0f; }});
                frontend.add_game_option({"analogCameraTiltSpeed", "Camera tilt speed",
                                          "Tilt speed at full stick deflection.",
                                          true, percent(g_tilt_speed), 50, 200, 10, "analogCamera",
                                          [](int v) { g_tilt_speed = static_cast<float>(v) / 100.0f; }});
                frontend.add_game_option({"mouseCamera", "Mouse camera",
                                          "The mouse controls the camera and bow and bowgun aiming. Press Esc to release the mouse.", false,
                                          g_mouse_camera ? 1 : 0, 0, 1, 1, "analogCamera",
                                          [](int v) { g_mouse_camera = v != 0; }});
                frontend.add_game_option({"mouseCameraSpeed", "Mouse sensitivity", "Camera sensitivity to mouse movement.",
                                          true, percent(g_mouse_speed), 25, 300, 25, "mouseCamera",
                                          [](int v) { g_mouse_speed = static_cast<float>(v) / 100.0f; }});
                // The mouse is held in the window while it moves the camera: both switches on.
                frontend.set_mouse_look([] { return g_analog_camera.load() && g_mouse_camera.load(); });
            }
            // Widescreen: the shape the frontend works out from the window reaches the game's camera at every poll.
            extra = [rt = &runtime, fe = &frontend, previous = std::move(extra)]() {
                keep_camera_aspect(*rt, fe->game_aspect());
                if (previous) previous();
            };
            frontend.attach(runtime, extra);
        } else if (window) {
            // A player asked for a window: never keep running invisibly.
            std::fprintf(stderr, "error: the game window could not be opened (see the messages above)\n");
            return 3;
        } else if (extra) {
            runtime.kernel_host_poll(extra);
        }
#else
        (void)keyboard; // only the windowed frontend maps the keyboard and the controller
        (void)controller;
        (void)controls_file;
        (void)friends_file;
        (void)friend_actions_file;
        (void)horn_songs;
        (void)horn_songs_place;
        if (window) std::fprintf(stderr, "[mhfu] built without the SDL frontend; running headless\n");
        if (extra) runtime.kernel_host_poll(extra);
#endif
        return runtime.run(*programs[0], "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
