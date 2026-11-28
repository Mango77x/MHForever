#include "shadow_geometry.hpp"

#include "check.hpp"

#include <numeric>
#include <vector>

int main() {
    using mhf::psp::shadow::KeptRange;
    using mhf::psp::shadow::pack_vertices;

    constexpr std::size_t stride = 5;
    std::vector<float> captured(24 * stride);
    std::iota(captured.begin(), captured.end(), 0.0f);
    std::vector<float> packed;

    // A filtered triangle appended to the capture is drawn before two neighbouring original ranges.
    std::vector<KeptRange> ranges{{.first = 21, .count = 3, .vertices = 42, .player = true},
                                  {.first = 6, .count = 3, .texture = 17},
                                  {.first = 9, .count = 6, .fixed = true}};
    pack_vertices(captured, ranges, packed);
    std::vector<float> expected(captured.begin() + 21 * stride, captured.end());
    expected.insert(expected.end(), captured.begin() + 6 * stride, captured.begin() + 15 * stride);
    MHF_CHECK(packed == expected);
    MHF_CHECK(ranges[0].first == 0 && ranges[0].count == 3 && ranges[0].player && ranges[0].vertices == 42);
    MHF_CHECK(ranges[1].first == 3 && ranges[1].count == 3 && ranges[1].texture == 17);
    MHF_CHECK(ranges[2].first == 6 && ranges[2].count == 6 && ranges[2].fixed);

    // All vertices are used, so the original layout can be copied intact even in reverse draw order.
    ranges = {{.first = 12, .count = 12}, {.first = 0, .count = 12}};
    pack_vertices(captured, ranges, packed);
    MHF_CHECK(packed == captured);
    MHF_CHECK(ranges[0].first == 12 && ranges[1].first == 0);

    // A retained buffer is reused across frames, including frames without casters.
    pack_vertices(captured, {}, packed);
    MHF_CHECK(packed.empty());
    return mhf::test::finish("shadow_geometry");
}
