#include "gathering_logic.hpp"

namespace mhf::mhfu {
GatheringDecision decide_gathering(const GatheringInput &in) {
    GatheringDecision out;
    if (!in.quest_active || in.menu_open || !in.ready || in.node_index >= 0xffff) return out;
    out.kind = in.carving          ? GatheringKind::carve
               : in.node_type == 3 ? GatheringKind::mining
               : in.node_type == 4 ? GatheringKind::bugs
                                   : GatheringKind::gather;
    if (out.kind == GatheringKind::gather || out.kind == GatheringKind::carve) {
        out.available = !in.exhausted;
        return out;
    }
    for (std::size_t i = 0; i < in.inventory.size() && i < 25; ++i) {
        const auto &item = in.inventory[i];
        const bool tool = out.kind == GatheringKind::mining
                              ? ((item.id >= 0x90 && item.id <= 0x92) || item.id == 0x4ca)
                              : ((item.id >= 0x94 && item.id <= 0x96) || item.id == 0x4c9);
        if (tool && item.count) {
            out.tool_slot = static_cast<std::uint16_t>(i);
            break;
        }
    }
    out.available = !in.exhausted && out.tool_slot.has_value();
    return out;
}
} // namespace mhf::mhfu
