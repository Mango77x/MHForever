#include "sysmem.hpp"

#include <algorithm>
#include <iterator>
#include <vector>

namespace mhf::psp {

namespace {
std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}
std::uint32_t align_down(std::uint32_t value, std::uint32_t alignment) { return value & ~(alignment - 1u); }
} // namespace

SysMem::SysMem(std::uint32_t ram_end) : ram_end_(ram_end) {}

std::optional<SysMem::Range> SysMem::partition_range(int partition) const {
    switch (partition) {
    case 1: return Range{kKernelPartitionStart, kUserPartitionStart};
    case 2: case 5: case 6: return Range{kUserPartitionStart, ram_end_};
    default: return std::nullopt;
    }
}

bool SysMem::overlaps(std::uint32_t start, std::uint32_t end) const {
    auto it = blocks_.upper_bound(start);
    if (it != blocks_.begin()) {
        const auto &prev = std::prev(it)->second;
        if (prev.address + prev.size > start) {
            return true;
        }
    }
    return it != blocks_.end() && it->first < end;
}

std::optional<SysMem::Block> SysMem::allocate(int partition, const std::string &name, Placement placement,
                                              std::uint32_t size, std::uint32_t address_or_alignment) {
    const auto range = partition_range(partition);
    if (!range || size == 0) {
        return std::nullopt;
    }
    const std::uint32_t alignment = placement == Placement::Address
                                        ? kAlignment
                                        : std::max(kAlignment, address_or_alignment);
    const std::uint32_t rounded = align_up(size, kAlignment);
    std::optional<std::uint32_t> chosen;
    if (placement == Placement::Address) {
        const std::uint32_t start = align_down(address_or_alignment, kAlignment);
        if (start >= range->start && start + rounded <= range->end && !overlaps(start, start + rounded)) {
            chosen = start;
        }
    } else {
        // Walk the gaps between blocks inside the partition.
        std::vector<Range> gaps;
        std::uint32_t cursor = range->start;
        for (const auto &[address, block] : blocks_) {
            if (address + block.size <= range->start || address >= range->end) {
                continue;
            }
            if (address > cursor) {
                gaps.push_back({cursor, address});
            }
            cursor = std::max(cursor, address + block.size);
        }
        if (cursor < range->end) {
            gaps.push_back({cursor, range->end});
        }
        if (placement == Placement::Low) {
            for (const auto &g : gaps) {
                const std::uint32_t start = align_up(g.start, alignment);
                if (start < g.end && g.end - start >= rounded) {
                    chosen = start;
                    break;
                }
            }
        } else {
            for (auto g = gaps.rbegin(); g != gaps.rend(); ++g) {
                if (g->end - g->start < rounded) {
                    continue;
                }
                const std::uint32_t start = align_down(g->end - rounded, alignment);
                if (start >= g->start) {
                    chosen = start;
                    break;
                }
            }
        }
    }
    if (!chosen) {
        return std::nullopt;
    }
    Block block;
    block.uid = uid_counter_ != nullptr ? (*uid_counter_)++ : own_counter_++;
    block.partition = partition;
    block.address = *chosen;
    block.size = rounded;
    block.name = name;
    blocks_.emplace(block.address, block);
    return block;
}

std::optional<SysMem::Block> SysMem::reserve(int partition, const std::string &name, std::uint32_t address,
                                             std::uint32_t size) {
    const std::uint32_t start = align_down(address, kAlignment);
    const std::uint32_t end = align_up(address + size, kAlignment);
    if (overlaps(start, end)) {
        return std::nullopt;
    }
    Block block;
    block.uid = uid_counter_ != nullptr ? (*uid_counter_)++ : own_counter_++;
    block.partition = partition;
    block.address = start;
    block.size = end - start;
    block.name = name;
    blocks_.emplace(start, block);
    return block;
}

bool SysMem::free(int uid) {
    for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
        if (it->second.uid == uid) {
            blocks_.erase(it);
            return true;
        }
    }
    return false;
}

const SysMem::Block *SysMem::find(int uid) const {
    for (const auto &[address, block] : blocks_) {
        if (block.uid == uid) {
            return &block;
        }
    }
    return nullptr;
}

std::uint32_t SysMem::max_free(int partition) const {
    const auto range = partition_range(partition);
    if (!range) {
        return 0;
    }
    std::uint32_t best = 0;
    std::uint32_t cursor = range->start;
    for (const auto &[address, block] : blocks_) {
        if (address + block.size <= range->start || address >= range->end) {
            continue;
        }
        if (address > cursor) {
            best = std::max(best, address - cursor);
        }
        cursor = std::max(cursor, address + block.size);
    }
    if (cursor < range->end) {
        best = std::max(best, range->end - cursor);
    }
    return best;
}

std::uint32_t SysMem::total_free(int partition) const {
    const auto range = partition_range(partition);
    if (!range) {
        return 0;
    }
    std::uint32_t used = 0;
    for (const auto &[address, block] : blocks_) {
        if (address >= range->start && address < range->end) {
            used += block.size;
        }
    }
    return (range->end - range->start) - used;
}

} // namespace mhf::psp
