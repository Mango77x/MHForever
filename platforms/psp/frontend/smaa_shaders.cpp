#include "smaa_shaders.hpp"

#include "AreaTex.h"
#include "SearchTex.h"

#include <string_view>

extern const unsigned char kSmaaHlsl[];
extern const std::size_t kSmaaHlslSize;

namespace mhf::psp::smaa {
namespace {

std::string_view hlsl_source() noexcept {
    return {reinterpret_cast<const char *>(kSmaaHlsl), kSmaaHlslSize};
}

std::string build_stage(bool is_vs, const char *entry) {
    const char *preamble = is_vs
        ? "#version 330 core\n"
          "#define SMAA_GLSL_3 1\n"
          "#define SMAA_PRESET_HIGH 1\n"
          "#define SMAA_INCLUDE_VS 1\n"
          "#define SMAA_INCLUDE_PS 0\n"
          "uniform vec4 u_smaa_metrics;\n"
          "#define SMAA_RT_METRICS u_smaa_metrics\n"
        : "#version 330 core\n"
          "#define SMAA_GLSL_3 1\n"
          "#define SMAA_PRESET_HIGH 1\n"
          "#define SMAA_INCLUDE_VS 0\n"
          "#define SMAA_INCLUDE_PS 1\n"
          "uniform vec4 u_smaa_metrics;\n"
          "#define SMAA_RT_METRICS u_smaa_metrics\n";
    const std::string_view body = hlsl_source();
    std::string out;
    out.reserve(body.size() + 1024u);
    out.append(preamble);
    out.append(body);
    out.push_back('\n');
    out.append(entry);
    return out;
}

constexpr const char *kEdgeVs = R"(
out vec2 v_uv;
out vec4 v_offset[3];
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv = p;
    SMAAEdgeDetectionVS(v_uv, v_offset);
}
)";

constexpr const char *kEdgePs = R"(
in vec2 v_uv;
in vec4 v_offset[3];
uniform sampler2D u_color;
out vec4 o_edges;
void main() {
    o_edges = vec4(SMAAColorEdgeDetectionPS(v_uv, v_offset, u_color), 0.0, 1.0);
}
)";

constexpr const char *kWeightVs = R"(
out vec2 v_uv;
out vec2 v_pixcoord;
out vec4 v_offset[3];
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv = p;
    SMAABlendingWeightCalculationVS(v_uv, v_pixcoord, v_offset);
}
)";

constexpr const char *kWeightPs = R"(
in vec2 v_uv;
in vec2 v_pixcoord;
in vec4 v_offset[3];
uniform sampler2D u_edges;
uniform sampler2D u_area;
uniform sampler2D u_search;
out vec4 o_weights;
void main() {
    o_weights = SMAABlendingWeightCalculationPS(v_uv, v_pixcoord, v_offset, u_edges, u_area, u_search, vec4(0.0));
}
)";

constexpr const char *kBlendVs = R"(
out vec2 v_uv;
out vec4 v_offset;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv = p;
    SMAANeighborhoodBlendingVS(v_uv, v_offset);
}
)";

constexpr const char *kBlendPs = R"(
in vec2 v_uv;
in vec4 v_offset;
uniform sampler2D u_color;
uniform sampler2D u_blend;
out vec4 o_color;
void main() {
    o_color = SMAANeighborhoodBlendingPS(v_uv, v_offset, u_color, u_blend);
}
)";

} // namespace

LutView area_tex() noexcept {
    return {areaTexBytes, AREATEX_WIDTH, AREATEX_HEIGHT, 2, AREATEX_SIZE};
}

LutView search_tex() noexcept {
    return {searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1, SEARCHTEX_SIZE};
}

std::string vertex_shader(Pass pass) {
    switch (pass) {
    case Pass::EdgeDetection: return build_stage(true, kEdgeVs);
    case Pass::BlendingWeight: return build_stage(true, kWeightVs);
    case Pass::NeighborhoodBlending: return build_stage(true, kBlendVs);
    }
    return {};
}

std::string fragment_shader(Pass pass) {
    switch (pass) {
    case Pass::EdgeDetection: return build_stage(false, kEdgePs);
    case Pass::BlendingWeight: return build_stage(false, kWeightPs);
    case Pass::NeighborhoodBlending: return build_stage(false, kBlendPs);
    }
    return {};
}

} // namespace mhf::psp::smaa
