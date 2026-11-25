#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mhf::psp::shadow {

struct KeptRange { // one run of triangles of a map, with what it takes to draw it again
    std::uint32_t first{}, count{}, vertices{};
    std::uint32_t texture{};
    float xform[4]{1, 1, 0, 0};
    std::int32_t func{1}, ref{}, mask{255}, clamp_s{}, clamp_t{};
    bool scenery{}, player{};
    bool fixed{}; // part of the scenery: it can be kept from one picture to the next
};

// Each range starts in captured and is updated to index the retained buffer. The draw order stays the same.
inline void pack_vertices(std::span<const float> captured, std::span<KeptRange> ranges, std::vector<float> &packed) {
    constexpr std::size_t stride = 5; // eye x, y, z and texture u, v
    std::size_t vertex_count = 0;
    for (const KeptRange &range : ranges) vertex_count += range.count;
    if (vertex_count * stride >= captured.size()) {
        // The original layout already fits in the same space, regardless of draw order.
        packed.assign(captured.begin(), captured.end());
        return;
    }
    packed.clear();
    packed.reserve(vertex_count * stride);

    std::uint32_t next = 0;
    for (std::size_t i = 0; i < ranges.size();) {
        // Neighbouring draws often use consecutive vertices. Copy the whole run together.
        const std::size_t first = ranges[i].first;
        std::size_t end = first;
        do {
            KeptRange &range = ranges[i++];
            range.first = next;
            next += range.count;
            end += range.count;
        } while (i < ranges.size() && ranges[i].first == end);
        const auto run = captured.subspan(first * stride, (end - first) * stride);
        packed.insert(packed.end(), run.begin(), run.end());
    }
}

} // namespace mhf::psp::shadow
