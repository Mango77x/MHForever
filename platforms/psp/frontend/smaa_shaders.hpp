#pragma once

// GLSL 330 Core shader assembly and precomputed LUT views for SMAA 1x (third_party/smaa, MIT).
// Kept free of SDL/OpenGL headers so unit tests can validate shader generation and LUTs without a display.

#include <cstddef>
#include <string>

namespace mhf::psp::smaa {

enum class Pass {
    EdgeDetection,
    BlendingWeight,
    NeighborhoodBlending,
};

struct LutView {
    const unsigned char *data{};
    int width{};
    int height{};
    int channels{};
    std::size_t size{};
};

LutView area_tex() noexcept;
LutView search_tex() noexcept;

std::string vertex_shader(Pass pass);
std::string fragment_shader(Pass pass);

} // namespace mhf::psp::smaa
