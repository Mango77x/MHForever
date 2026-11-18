#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace mhf::mhfu {
enum class GatheringKind { none, gather, mining, bugs, carve };
struct GatheringItem {
    std::uint16_t id{}, count{};
};
struct GatheringInput {
    std::uint32_t node_index = 0xffff;
    std::uint16_t node_type{};
    std::span<const GatheringItem> inventory;
    bool exhausted = false, carving = false;
};
struct GatheringDecision {
    GatheringKind kind = GatheringKind::none;
    std::optional<std::uint16_t> tool_slot;
    bool available = false;
};
GatheringDecision decide_gathering(const GatheringInput &input);
} // namespace mhf::mhfu
