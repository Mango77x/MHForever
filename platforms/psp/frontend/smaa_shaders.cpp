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

// Strip block comments and normalize line endings so strict GLSL 330 compilers do not reject
// trailing '\' characters in SMAA.hlsl's ASCII banner as GLSL 420+ line continuations, while
// keeping third_party/smaa/SMAA.hlsl verbatim upstream. Preserves newlines for diagnostics.
std::string sanitize_glsl330(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    bool in_line_comment = false;
    bool in_block_comment = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char next = i + 1 < src.size() ? src[i + 1] : '\0';
        if (in_block_comment) {
            if (c == '*' && next == '/') {
                in_block_comment = false;
                ++i;
                out.push_back(' ');
            } else if (c == '\n') {
                out.push_back('\n');
            }
            continue;
        }
        if (in_line_comment) {
            if (c == '\n') {
                in_line_comment = false;
                while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
                while (!out.empty() && out.back() == '\\') out.pop_back();
                out.push_back('\n');
            } else if (c != '\r') {
                out.push_back(c);
            }
            continue;
        }
        if (c == '/' && next == '*') {
            in_block_comment = true;
            ++i;
            continue;
        }
        if (c == '/' && next == '/') {
            in_line_comment = true;
            out.append("//");
            ++i;
            continue;
        }
        if (c != '\r') out.push_back(c);
    }
    return out;
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
    const std::string body = sanitize_glsl330(hlsl_source());
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
