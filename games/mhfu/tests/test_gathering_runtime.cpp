// Synthetic guest functions and memory only: no executable, ISO or save data
// needed.
#include "check.hpp"
#include "gathering.hpp"
#include "mhf/psp/dialog.hpp"
#include "mhf/psp/host.hpp"
#include "mhfu_region_data.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
using namespace mhf::psp;
namespace region = mhf::psp::generated::mhfu_region_data;
namespace {
constexpr std::uint32_t sys = 0x08810000, manager = 0x08890000, player = 0x08891000, bag = 0x08893000;
int probes = 0, actions = 0;
bool static_target = true;
std::uint16_t probed_type = 3;
constexpr std::uint32_t nodes = 0x08897000, dynamic_nodes = 0x08898000;
bool reject_action = false, throw_action = false;
std::uint8_t started_action = 0x51;
void probe(Runtime &rt, Context &c) {
    ++probes;
    MHF_CHECK(c.gpr[4] == player);
    rt.memory().store16(c.gpr[5], probed_type);
    c.gpr[2] = static_target ? 0 : 0xffff;
    c.gpr[16] = 0xdeadbeef;
}
void stage_nodes(Runtime &, Context &c) { c.gpr[2] = nodes; }
void inventory(Runtime &, Context &c) {
    MHF_CHECK(c.gpr[4] == sys);
    c.gpr[2] = bag;
    c.gpr[6] = 0xbad;
}
void interact(Runtime &rt, Context &c) {
    if (throw_action) throw std::runtime_error("synthetic interaction failure");
    if (reject_action) return;
    ++actions;
    MHF_CHECK(c.gpr[4] == player && c.gpr[5] == 2);
    MHF_CHECK(rt.memory().load16(player + 0x648) == 2);
    rt.memory().store8(player + 0x299, started_action);
    c.gpr[16] = 123;
}
} // namespace
int main() {
    RuntimeOptions options;
    options.interactive_dialogs = true;
    Runtime rt(options);
    if (!region::mh_gathering_probe || !region::mh_gathering_interact || !region::mh_inventory_slot) {
        mhf::mhfu::Gathering unmapped;
        unmapped.indicators = true;
        unmapped.automatic_tools = true;
        Context untouched{};
        const auto before = untouched;
        unmapped.update(rt, untouched);
        MHF_CHECK(unmapped.hint(rt).empty());
        MHF_CHECK(std::memcmp(&untouched, &before, sizeof untouched) == 0);
        return mhf::test::finish("test_gathering_runtime_unmapped");
    }
    std::array<FunctionEntry, 2> f{{{*region::mh_gathering_probe, probe}, {*region::mh_gathering_interact, interact}}};
    std::array<FunctionEntry, 2> main_functions{
        {{*region::mh_inventory_slot, inventory}, {*region::mh_stage_gathering_nodes, stage_nodes}}};
    const Program main_program{"synthetic", 0, 0, main_functions, {}, 0x08804000, 0x08910000, -1, {}};
    rt.add_program(main_program);
    const auto overlay_begin = std::min(*region::mh_gathering_probe, *region::mh_gathering_interact) & ~0xfffu;
    const auto overlay_end = std::max(*region::mh_gathering_probe, *region::mh_gathering_interact) + 0x1000u;
    const Program overlay{"synthetic_ovl_game_task", 0, 0, f, {}, overlay_begin, overlay_end, 71, {}};
    rt.add_overlay(overlay);
    MHF_CHECK(rt.activate_overlay(71));
    auto &m = rt.memory();
    m.store32(*region::mh_stage_manager, sys);
    m.store32(*region::mh_gathering_dynamic_nodes, dynamic_nodes);
    for (unsigned i = 0; i < 20; ++i)
        m.store16(dynamic_nodes + 4 + i * 32 + 16, std::uint16_t{0xffff});
    m.store16(nodes + 16, std::uint16_t{0});
    m.store16(nodes + 18, std::uint16_t{255});
    m.store16(nodes + 20, std::uint16_t{3});
    m.store32(nodes + 24, std::uint32_t{0xbf800000});
    m.store32(*region::mh_game_sys, sys);
    m.store32(*region::mh_player_manager, manager);
    m.store32(*region::mh_cockpit, std::uint32_t{0});
    m.store8(sys + 0x28, std::uint8_t{0});
    m.store32(manager + 0x0c, player);
    m.store16(bag + 8, std::uint16_t{0x91});
    m.store16(bag + 10, std::uint16_t{2});
    Context ctx{};
    ctx.gpr[29] = 0x09ff0000;
    ctx.gpr[16] = 42;
    const auto saved = ctx;
    mhf::mhfu::Gathering g;
    g.indicators = true;
    g.automatic_tools = true;
    g.language = 3;
    mhf::mhfu::GatheringArt art;
    for (auto &image : art) {
        image.size = 1;
        image.pixels = std::make_shared<const std::vector<std::uint32_t>>(1, 0);
    }
    g.set_icons(std::move(art));
    set_controller_state(rt, {});
    g.update(rt, ctx);
    MHF_CHECK(probes == 0 && actions == 0 && g.hint(rt).empty()); // invalid cockpit must fail closed
    m.store32(*region::mh_cockpit, std::uint32_t{0x08894000});
    g.update(rt, ctx);
    MHF_CHECK(probes == 1 && actions == 0);
    MHF_CHECK(g.hint(rt) == "○  Minar");
    ControllerState pad{};
    pad.buttons = 0x2000;
    set_controller_state(rt, pad);
    g.update(rt, ctx);
    MHF_CHECK(actions == 1);
    MHF_CHECK(std::memcmp(&ctx, &saved, sizeof ctx) == 0);
    g.update(rt, ctx);
    MHF_CHECK(actions == 1);
    set_controller_state(rt, {});
    m.store8(player + 0x299, std::uint8_t{0});
    g.update(rt, ctx);
    MHF_CHECK(m.load16(player + 0x648) == 0); // previous selection restored after the action
    m.store16(bag + 10, std::uint16_t{0});
    set_controller_state(rt, pad);
    g.update(rt, ctx);
    MHF_CHECK(actions == 1);
    MHF_CHECK(g.hint(rt) == "Minar - Sin herramienta");
    // A rejected stock interaction must not change the selected item.
    set_controller_state(rt, {});
    g.update(rt, ctx);
    m.store16(bag + 10, std::uint16_t{2});
    m.store16(player + 0x648, std::uint16_t{4});
    reject_action = true;
    set_controller_state(rt, pad);
    g.update(rt, ctx);
    MHF_CHECK(m.load16(player + 0x648) == 4 && actions == 1);
    reject_action = false;
    m.store8(sys + 0x10, std::uint8_t{1});
    g.update(rt, ctx);
    MHF_CHECK(g.hint(rt).empty());
    m.store8(sys + 0x10, std::uint8_t{0});
    m.store32(manager + 0x0c, std::uint32_t{0});
    g.update(rt, ctx);
    MHF_CHECK(g.hint(rt).empty());
    // Dismissing the frontend menu with Circle must consume that press until
    // release.
    m.store32(manager + 0x0c, player);
    m.store8(player + 0x299, std::uint8_t{0});
    bool frontend_blocked = true;
    g.input_blocked = [&] { return frontend_blocked; };
    set_controller_state(rt, {});
    g.update(rt, ctx);
    set_controller_state(rt, pad);
    g.update(rt, ctx);
    frontend_blocked = false;
    g.update(rt, ctx);
    MHF_CHECK(actions == 1);
    set_controller_state(rt, {});
    g.update(rt, ctx);
    set_controller_state(rt, pad);
    g.update(rt, ctx);
    MHF_CHECK(actions == 2);
    m.store8(player + 0x299, std::uint8_t{0});
    // Carving lives in the dynamic array, not in the original static-node probe.
    static_target = false;
    const auto corpse = dynamic_nodes + 4;
    m.store16(corpse + 16, std::uint16_t{1});
    m.store16(corpse + 18, std::uint16_t{2});
    m.store16(corpse + 20, std::uint16_t{2});
    m.store32(corpse + 12, std::uint32_t{0x42c80000}); // radius100
    set_controller_state(rt, {});
    g.update(rt, ctx);
    MHF_CHECK(g.hint(rt) == "○  Tallar");
    for (int i = 0; i < 5; ++i)
        g.update(rt, ctx);
    auto carve_sprites = g.sprites(rt);
    MHF_CHECK(carve_sprites.size() == 3);
    MHF_CHECK(std::memcmp(&ctx, &saved, sizeof ctx) == 0);
    m.store16(corpse + 18, std::uint16_t{0});
    g.update(rt, ctx);
    MHF_CHECK(g.hint(rt) == "Tallar - Agotado");
    auto depleted_sprites = g.sprites(rt);
    MHF_CHECK(depleted_sprites.size() == 3);
    if (carve_sprites.size() == 3 && depleted_sprites.size() == 3)
        MHF_CHECK(carve_sprites[2].pixels != depleted_sprites[2].pixels);
    // An exhausted target must not hide another carve within reach.
    const auto tail = corpse + 32;
    m.store16(tail + 16, std::uint16_t{2});
    m.store16(tail + 18, std::uint16_t{1});
    m.store16(tail + 20, std::uint16_t{2});
    m.store32(tail + 12, std::uint32_t{0x42c80000});
    g.update(rt, ctx);
    MHF_CHECK(g.hint(rt) == "○  Tallar");
    m.store16(tail + 24, std::uint16_t{1});
    g.update(rt, ctx);
    MHF_CHECK(g.hint(rt) == "Tallar - Agotado");
    m.store16(tail + 16, std::uint16_t{0xffff});
    m.store32(player + 0x200, std::uint32_t{0x43fa0000}); // x500 outside radius
    g.update(rt, ctx);
    MHF_CHECK(g.hint(rt).empty());
    MHF_CHECK(g.sprites(rt).empty()); // no residual hexagon after leaving a target
    for (int i = 0; i < 10; ++i)
        g.update(rt, ctx);
    MHF_CHECK(g.sprites(rt).empty()); // also hidden at the old minimum alpha
    m.store32(player + 0x200, std::uint32_t{0});
    m.store16(corpse + 24, std::uint16_t{1}); // other area
    g.update(rt, ctx);
    MHF_CHECK(g.hint(rt).empty());
    m.store16(corpse + 24, std::uint16_t{0});
    m.store16(corpse + 16, std::uint16_t{0xffff}); // despawned
    g.update(rt, ctx);
    MHF_CHECK(g.hint(rt).empty());
    static_target = true;
    m.store16(nodes + 18, std::uint16_t{0});
    g.update(rt, ctx);
    set_controller_state(rt, pad);
    g.update(rt, ctx);
    MHF_CHECK(actions == 2 && g.hint(rt) == "Minar - Agotado");

    // A depleted mining spot can overlap a corpse the hunter can still carve.
    m.store16(corpse + 16, std::uint16_t{1});
    m.store16(corpse + 18, std::uint16_t{1});
    g.update(rt, ctx);
    MHF_CHECK(actions == 2 && g.hint(rt) == "○  Tallar");
    m.store16(corpse + 18, std::uint16_t{0});
    g.update(rt, ctx);
    MHF_CHECK(g.hint(rt) == "Minar - Agotado");
    m.store16(nodes + 18, std::uint16_t{255});
    m.store16(corpse + 18, std::uint16_t{1});
    g.update(rt, ctx);
    MHF_CHECK(actions == 2 && g.hint(rt) == "○  Minar");
    m.store16(corpse + 16, std::uint16_t{0xffff});

    // Consumption can happen after the start call. Keep the tool until the action ends, including when it breaks.
    g.input_blocked = {};
    for (std::uint16_t type : {std::uint16_t{3}, std::uint16_t{4}}) {
        started_action = type == 3 ? std::uint8_t{0x51} : std::uint8_t{0x52};
        probed_type = type;
        m.store16(nodes + 20, type);
        for (int scenario = 0; scenario < 7; ++scenario) {
            mhf::mhfu::Gathering restore;
            restore.indicators = restore.automatic_tools = true;
            m.store32(manager + 0x0c, player);
            m.store8(player + 0x299, std::uint8_t{0});
            m.store16(player + 0x648, std::uint16_t{4});
            m.store16(bag + 8, type == 3 ? std::uint16_t{0x91} : std::uint16_t{0x95});
            m.store16(bag + 10, std::uint16_t{2});
            set_controller_state(rt, {});
            restore.update(rt, ctx);
            set_controller_state(rt, pad);
            const auto before_action = actions;
            throw_action = scenario == 5;
            try {
                restore.update(rt, ctx);
                MHF_CHECK(!throw_action);
            } catch (const std::runtime_error &) {
                MHF_CHECK(throw_action);
            }
            throw_action = false;
            if (scenario == 5) {
                MHF_CHECK(actions == before_action && m.load16(player + 0x648) == 4);
                continue;
            }
            MHF_CHECK(actions == before_action + 1 && m.load16(player + 0x648) == 2);
            restore.update(rt, ctx);
            MHF_CHECK(actions == before_action + 1 && m.load16(player + 0x648) == 2);
            if (scenario == 6) {
                // Selecting another item and then the tool again is still a deliberate selection.
                m.store16(player + 0x648, std::uint16_t{6});
                restore.update(rt, ctx);
                m.store16(player + 0x648, std::uint16_t{2});
                restore.update(rt, ctx);
            } else {
                m.store16(bag + 10, std::uint16_t{0}); // the last tool breaks during the animation
            }
            if (scenario == 1) m.store16(player + 0x648, std::uint16_t{6}); // another selection wins
            if (scenario == 2) restore.indicators = restore.automatic_tools = false;
            if (scenario == 3) restore.input_blocked = [] { return true; };
            if (scenario == 4) {
                m.store32(manager + 0x0c, std::uint32_t{0});
                restore.update(rt, ctx); // leaving the player discards the pending write
                m.store32(manager + 0x0c, player);
            }
            m.store8(player + 0x299, std::uint8_t{0});
            set_controller_state(rt, {});
            restore.update(rt, ctx);
            MHF_CHECK(m.load16(player + 0x648) == (scenario == 1 ? 6 : (scenario == 4 || scenario == 6) ? 2 : 4));
            MHF_CHECK(std::memcmp(&ctx, &saved, sizeof ctx) == 0);
        }
    }
    started_action = 0x51;
    probed_type = 3;
    m.store16(nodes + 20, std::uint16_t{3});
    m.store16(bag + 8, std::uint16_t{0x91});
    const auto actions_before_dialog = actions;
    // Text-only fallback for every supported system language, including the non-ASCII glyphs.
    const std::array<std::array<const char *, 6>, 6> labels{{
        {"採掘", "虫捕り", "採集", "剥ぎ取り", "採集済み", "道具なし"},
        {"Mine", "Catch bugs", "Gather", "Carve", "Depleted", "No tool"},
        {"Miner", "Attraper des insectes", "Ramasser", "Dépecer", "Épuisé", "Sans outil"},
        {"Minar", "Atrapar insectos", "Recoger", "Tallar", "Agotado", "Sin herramienta"},
        {"Abbauen", "Käfer fangen", "Sammeln", "Ausweiden", "Erschöpft", "Kein Werkzeug"},
        {"Estrai", "Cattura insetti", "Raccogli", "Scuoia", "Esaurito", "Nessun attrezzo"},
    }};
    for (int language = 0; language < 6; ++language) {
        mhf::mhfu::Gathering localized;
        localized.indicators = localized.automatic_tools = true;
        localized.language = language;
        const auto &text = labels[static_cast<std::size_t>(language)];
        static_target = true;
        probed_type = 3;
        m.store16(nodes + 20, std::uint16_t{3});
        m.store16(nodes + 18, std::uint16_t{255});
        m.store16(bag + 8, std::uint16_t{0x91});
        m.store16(bag + 10, std::uint16_t{2});
        set_controller_state(rt, {});
        localized.update(rt, ctx);
        MHF_CHECK(localized.hint(rt) == std::string("○  ") + text[0]);
        m.store16(nodes + 18, std::uint16_t{0});
        localized.update(rt, ctx);
        MHF_CHECK(localized.hint(rt) == std::string(text[0]) + " - " + text[4]);
        m.store16(nodes + 18, std::uint16_t{255});
        m.store16(bag + 10, std::uint16_t{0});
        localized.update(rt, ctx);
        MHF_CHECK(localized.hint(rt) == std::string(text[0]) + " - " + text[5]);
        probed_type = 4;
        m.store16(nodes + 20, std::uint16_t{4});
        m.store16(bag + 8, std::uint16_t{0x95});
        m.store16(bag + 10, std::uint16_t{2});
        localized.update(rt, ctx);
        MHF_CHECK(localized.hint(rt) == std::string("○  ") + text[1]);
        probed_type = 0;
        m.store16(nodes + 20, std::uint16_t{0});
        localized.update(rt, ctx);
        MHF_CHECK(localized.hint(rt) == std::string("○  ") + text[2]);
        static_target = false;
        m.store16(corpse + 16, std::uint16_t{1});
        m.store16(corpse + 18, std::uint16_t{2});
        localized.update(rt, ctx);
        MHF_CHECK(localized.hint(rt) == std::string("○  ") + text[3]);
        MHF_CHECK(localized.sprites(rt).empty());
        m.store16(corpse + 16, std::uint16_t{0xffff});
    }
    static_target = true;
    probed_type = 0;
    m.store16(nodes + 20, std::uint16_t{0});
    for (int language : {-1, 6, 100}) {
        mhf::mhfu::Gathering localized;
        localized.indicators = true;
        localized.language = language;
        localized.update(rt, ctx);
        MHF_CHECK(localized.hint(rt) == "○  Gather");
    }
    // Raw controller input must not leak into gameplay during a system dialog.
    m.store32(manager + 0x0c, player);
    set_controller_state(rt, {});
    g.update(rt, ctx);
    constexpr std::uint32_t dialog_params = 0x08896000;
    m.store32(dialog_params + 0x34, std::uint32_t{1});
    auto message = ctx;
    message.gpr[4] = dialog_params;
    rt.find_hle("sceUtility", 0x2AD8E239u)->handler(rt, message);
    MHF_CHECK(current_dialog(rt).kind == DialogView::Kind::Message);
    const auto probes_before = probes;
    set_controller_state(rt, pad);
    g.update(rt, ctx);
    MHF_CHECK(probes == probes_before && actions == actions_before_dialog && g.hint(rt).empty());
    return mhf::test::finish("test_gathering_runtime");
}
