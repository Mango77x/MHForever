#include "gathering.hpp"
#include "gathering_logic.hpp"
#include "mhf/psp/dialog.hpp"
#include "mhf/psp/host.hpp"
#include "mhfu_region_data.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <string_view>

namespace mhf::mhfu {
namespace region = psp::generated::mhfu_region_data;
namespace {

constexpr std::uint32_t kGameSysPlayerSlot = 0x28, kGameSysMenu = 0x10;
constexpr std::uint32_t kPlayerManagerPlayers = 0x0C, kPlayerSlots = 6;
constexpr std::uint32_t kCockpitMenu = 0x604, kCockpitWindow = 0x52F;
constexpr std::uint32_t kPlayerPosition = 0x200, kPlayerArea = 0x29A, kPlayerReady = 0x325;
constexpr std::uint32_t kPlayerAction = 0x299, kPlayerSelectedItem = 0x648;
constexpr std::uint32_t kNodeId = 16, kNodeRemaining = 18, kNodeType = 20;
constexpr std::uint32_t kDynamicNodes = 20, kDynamicNodeSize = 32, kStaticNodeSize = 24;
constexpr std::uint32_t kInventorySlots = 25, kCircle = 0x2000;
constexpr std::uint8_t kMiningAction = 0x51, kBugAction = 0x52;
constexpr std::uint64_t kHintLifetimeUs = 150'000;

struct HintText {
    const char *mine, *bugs, *gather, *carve, *depleted, *no_tool;
};
constexpr std::array<HintText, 6> kHints{{
    {"採掘", "虫捕り", "採集", "剥ぎ取り", "採集済み", "道具なし"},
    {"Mine", "Catch bugs", "Gather", "Carve", "Depleted", "No tool"},
    {"Miner", "Attraper des insectes", "Ramasser", "Dépecer", "Épuisé", "Sans outil"},
    {"Minar", "Atrapar insectos", "Recoger", "Tallar", "Agotado", "Sin herramienta"},
    {"Abbauen", "Käfer fangen", "Sammeln", "Ausweiden", "Erschöpft", "Kein Werkzeug"},
    {"Estrai", "Cattura insetti", "Raccogli", "Scuoia", "Esaurito", "Nessun attrezzo"},
}};

struct Target {
    std::uint32_t index;
    std::uint16_t type, remaining;
    bool carving;
};

// Read-only: gathering checks and inventory access must preserve the interrupted guest context.
std::optional<Target> read_target(psp::Runtime &rt, const psp::Context &ctx, std::uint32_t player,
                                  std::uint32_t scratch) {
    auto &m = rt.memory();
    auto query = ctx;
    query.gpr[4] = player;
    query.gpr[5] = scratch + 12;
    query.gpr[6] = scratch;
    m.store16(scratch + 12, std::uint16_t{0xffff});
    rt.call(query, *region::mh_gathering_probe);
    const auto index = query.gpr[2] & 0xffffu;
    const auto type = m.load16(scratch + 12);
    if (index == 0xffffu) {
        // The stock carving query scans these dynamic corpse/tail targets.
        // Read only, with bounds/finite checks; never invoke reward/consumption
        // routines.
        const auto targets = m.load32(*region::mh_gathering_dynamic_nodes);
        if (!m.contains(targets, 4 + kDynamicNodes * kDynamicNodeSize)) return {};
        const auto number = [](std::uint32_t bits) { return std::bit_cast<float>(bits); };
        const float px = number(m.load32(player + kPlayerPosition));
        const float py = number(m.load32(player + kPlayerPosition + 4));
        const float pz = number(m.load32(player + kPlayerPosition + 8));
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) return {};
        std::optional<Target> exhausted_carve;
        for (unsigned i = 0; i < kDynamicNodes; ++i) {
            const auto node = targets + 4 + i * kDynamicNodeSize;
            if (m.load16(node + kNodeId) == 0xffff || m.load16(node + 24) != m.load16(player + kPlayerArea)) continue;
            const float x = number(m.load32(node)), y = number(m.load32(node + 4));
            const float z = number(m.load32(node + 8)), radius = number(m.load32(node + 12));
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(radius) || radius < 0 ||
                py < y - 200 || py >= y + 100 || (px - x) * (px - x) + (pz - z) * (pz - z) > radius * radius)
                continue;
            const auto node_type = m.load16(node + kNodeType);
            const Target target{m.load16(node + kNodeId), node_type, m.load16(node + kNodeRemaining), node_type == 2};
            if (target.carving && target.remaining != 0) return target;
            // Keep the first exhausted carve as a fallback if no other carve is available.
            if (!exhausted_carve) {
                if (!target.carving) return target;
                exhausted_carve = target;
            }
        }
        return exhausted_carve;
    } else {
        // The static probe ignores the remaining-use field; inspect the matched
        // node.
        const auto stage = m.load32(*region::mh_stage_manager);
        if (!m.contains(stage, 0x64)) return {};
        auto list_call = ctx;
        list_call.gpr[4] = stage;
        list_call.gpr[5] = m.load16(player + kPlayerArea);
        rt.call(list_call, *region::mh_stage_gathering_nodes);
        auto node = list_call.gpr[2];
        for (unsigned i = 0; i < 512 && m.contains(node, kStaticNodeSize); ++i, node += kStaticNodeSize) {
            if (m.load32(node) == 0xbf800000u) break; // x=-1 sentinel
            if (m.load16(node + kNodeId) != index || m.load16(node + kNodeType) != type) continue;
            if (m.load32(node) != m.load32(scratch) || m.load32(node + 4) != m.load32(scratch + 4) ||
                m.load32(node + 8) != m.load32(scratch + 8))
                continue;
            return Target{index, type, m.load16(node + kNodeRemaining), false};
        }
        return {};
    }
}

} // namespace

void Gathering::update(psp::Runtime &rt, psp::Context &ctx) {
    if (busy_) return;
    visual_active_ = false;
    struct Fade {
        std::uint8_t &alpha;
        const bool &active;
        ~Fade() { alpha = gathering_alpha(alpha, active); }
    } fade{alpha_, visual_active_};
    const bool circle = (psp::controller_state(rt).buttons & kCircle) != 0;
    const bool pressed = circle && !held_;
    held_ = circle;
    hint_.clear();
    updated_ = rt.now_us();
    if constexpr (region::mh_gathering_probe && region::mh_gathering_interact && region::mh_inventory_slot &&
                  region::mh_game_sys && region::mh_player_manager && region::mh_cockpit &&
                  region::mh_gathering_dynamic_nodes && region::mh_stage_manager && region::mh_stage_gathering_nodes) {
        const auto *overlay = rt.active_overlay_at(*region::mh_gathering_probe);
        if (!overlay || !std::string_view(overlay->name).ends_with("_ovl_game_task")) {
            selection_.reset();
            return;
        }
        auto &m = rt.memory();
        const auto sys = m.load32(*region::mh_game_sys), manager = m.load32(*region::mh_player_manager);
        if (!m.contains(sys, 0x29) || !m.contains(manager, 0x24)) {
            selection_.reset();
            return;
        }
        const auto slot = m.load8(sys + kGameSysPlayerSlot);
        if (slot >= kPlayerSlots) {
            selection_.reset();
            return;
        }
        const auto player = m.load32(manager + kPlayerManagerPlayers + slot * 4u);
        if (!m.contains(player, 0x696)) {
            selection_.reset();
            return;
        }
        // Keep the tool selected until the game has finished consuming it. Never undo a later selection or write
        // to a player that has left. This also runs after assistance is turned off or a menu is opened.
        if (selection_) {
            if (selection_->player != player || m.load8(player + kPlayerReady) > 1 ||
                m.load16(player + kPlayerSelectedItem) != selection_->tool) {
                selection_.reset();
            } else if (m.load8(player + kPlayerAction) != selection_->action) {
                m.store16(player + kPlayerSelectedItem, selection_->previous);
                selection_.reset();
            }
        }
        if (!indicators && !automatic_tools) return;
        if ((input_blocked && input_blocked()) || psp::current_dialog(rt).kind != psp::DialogView::Kind::None) return;
        const auto cockpit = m.load32(*region::mh_cockpit);
        if (!m.contains(cockpit, 0x605) || m.load8(sys + kGameSysMenu) != 0 || m.load8(cockpit + kCockpitMenu) != 0 ||
            m.load8(cockpit + kCockpitWindow) != 0)
            return;
        if (m.load8(player + kPlayerReady) > 1) return;
        if (!scratch_) {
            const auto block = rt.allocate_kernel_block("gathering-query", 16);
            if (!block) return;
            scratch_ = *block;
        }
        busy_ = true;
        struct Guard {
            bool &b;
            ~Guard() { b = false; }
        } guard{busy_};
        // Every guest call gets a copy: probe/accessor/interaction must not clobber
        // the interrupted game's context.
        const auto target = read_target(rt, ctx, player, scratch_);
        if (!target) return;
        const auto [index, type, remaining, carving] = *target;
        auto bag_call = ctx;
        bag_call.gpr[4] = sys;
        bag_call.gpr[5] = 0;
        rt.call(bag_call, *region::mh_inventory_slot);
        const auto bag = bag_call.gpr[2];
        if (!m.contains(bag, kInventorySlots * 4)) return;
        std::array<GatheringItem, kInventorySlots> items{};
        for (std::uint32_t i = 0; i < kInventorySlots; ++i)
            items[i] = {m.load16(bag + 4u * i), m.load16(bag + 4u * i + 2)};
        const auto decision = decide_gathering({index, type, items, remaining == 0, carving});
        visual_active_ = decision.kind != GatheringKind::none;
        kind_ = decision.kind;
        unavailable_ = !decision.available;
        button_ = decision.available && (kind_ == GatheringKind::gather || kind_ == GatheringKind::carve ||
                                         (automatic_tools && decision.tool_slot.has_value()));
        if (indicators) {
            const auto &text = kHints[language >= 0 && static_cast<std::size_t>(language) < kHints.size()
                                         ? static_cast<std::size_t>(language) : 1u];
            if (decision.kind == GatheringKind::mining)
                hint_ = text.mine;
            else if (decision.kind == GatheringKind::bugs)
                hint_ = text.bugs;
            else if (decision.kind == GatheringKind::gather)
                hint_ = text.gather;
            else if (decision.kind == GatheringKind::carve)
                hint_ = text.carve;
            if (remaining == 0) {
                hint_.append(" - ").append(text.depleted);
            } else if (decision.kind == GatheringKind::mining || decision.kind == GatheringKind::bugs) {
                if (!decision.tool_slot)
                    hint_.append(" - ").append(text.no_tool);
                else if (automatic_tools)
                    hint_ = "○  " + hint_;
            } else if (!hint_.empty())
                hint_ = "○  " + hint_;
        }
        if (!automatic_tools || !pressed || !decision.tool_slot || !decision.available) return;
        const auto action = m.load8(player + kPlayerAction);
        if (action == kMiningAction || action == kBugAction) return;
        const auto previous_slot = m.load16(player + kPlayerSelectedItem);
        m.store16(player + kPlayerSelectedItem, *decision.tool_slot);
        auto interact = ctx;
        interact.gpr[4] = player;
        interact.gpr[5] = 2;
        try {
            rt.call(interact, *region::mh_gathering_interact);
        } catch (...) {
            m.store16(player + kPlayerSelectedItem, previous_slot);
            throw;
        }
        const auto expected_action = decision.kind == GatheringKind::mining ? kMiningAction : kBugAction;
        if (m.load8(player + kPlayerAction) != expected_action)
            m.store16(player + kPlayerSelectedItem, previous_slot);
        else
            selection_ = Selection{player, previous_slot, *decision.tool_slot, expected_action};
    }
}
std::string Gathering::hint(psp::Runtime &rt) const {
    return indicators && rt.now_us() - updated_ < kHintLifetimeUs ? hint_ : std::string{};
}
std::vector<overlay::HintSprite> Gathering::sprites(psp::Runtime &rt) const {
    if (!indicators || !visual_active_ || !art_ || rt.now_us() - updated_ >= kHintLifetimeUs)
        return {};
    return gathering_sprites(*art_, kind_, alpha_, button_, unavailable_);
}
} // namespace mhf::mhfu
