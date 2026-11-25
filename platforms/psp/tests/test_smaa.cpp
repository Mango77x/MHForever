// SMAA shader assembly and LUT integrity checks (frontend/smaa_shaders.*): runs without OpenGL or game data.

#include "smaa_shaders.hpp"

#include "check.hpp"

#include <algorithm>
#include <string>

using namespace mhf::psp::smaa;

int main() {
    // AreaTex: 160x560 RG8 (2 bytes/texel), non-trivial LUT content.
    const LutView area = area_tex();
    MHF_CHECK(area.data != nullptr);
    MHF_CHECK(area.width == 160);
    MHF_CHECK(area.height == 560);
    MHF_CHECK(area.channels == 2);
    MHF_CHECK(area.size == 160u * 560u * 2u);
    MHF_CHECK(area.data != nullptr && std::any_of(area.data, area.data + area.size, [](unsigned char b) { return b != 0; }));

    // SearchTex: 64x16 R8 (1 byte/texel), expected first sentinel byte from iryoku/smaa.
    const LutView search = search_tex();
    MHF_CHECK(search.data != nullptr);
    MHF_CHECK(search.width == 64);
    MHF_CHECK(search.height == 16);
    MHF_CHECK(search.channels == 1);
    MHF_CHECK(search.size == 64u * 16u);
    MHF_CHECK(search.data != nullptr && search.data[0] == 0xFEu);

    // Every pass produces GLSL 330 Core vertex and fragment shaders wrapping embedded SMAA.hlsl.
    const struct {
        Pass pass;
        const char *vs_call;
        const char *ps_call;
    } passes[] = {
        {Pass::EdgeDetection, "SMAAEdgeDetectionVS(", "SMAAColorEdgeDetectionPS("},
        {Pass::BlendingWeight, "SMAABlendingWeightCalculationVS(", "SMAABlendingWeightCalculationPS("},
        {Pass::NeighborhoodBlending, "SMAANeighborhoodBlendingVS(", "SMAANeighborhoodBlendingPS("},
    };
    for (const auto &p : passes) {
        const std::string vs = vertex_shader(p.pass);
        const std::string ps = fragment_shader(p.pass);
        MHF_CHECK(vs.starts_with("#version 330 core\n"));
        MHF_CHECK(vs.find("#define SMAA_GLSL_3 1") != std::string::npos);
        MHF_CHECK(vs.find("#define SMAA_PRESET_HIGH 1") != std::string::npos);
        MHF_CHECK(vs.find("#define SMAA_INCLUDE_VS 1") != std::string::npos);
        MHF_CHECK(vs.find("#define SMAA_INCLUDE_PS 0") != std::string::npos);
        MHF_CHECK(vs.find("u_smaa_metrics") != std::string::npos);
        MHF_CHECK(vs.find(p.vs_call) != std::string::npos);
        MHF_CHECK(vs.size() > 50000u); // embeds SMAA.hlsl

        MHF_CHECK(ps.starts_with("#version 330 core\n"));
        MHF_CHECK(ps.find("#define SMAA_GLSL_3 1") != std::string::npos);
        MHF_CHECK(ps.find("#define SMAA_PRESET_HIGH 1") != std::string::npos);
        MHF_CHECK(ps.find("#define SMAA_INCLUDE_VS 0") != std::string::npos);
        MHF_CHECK(ps.find("#define SMAA_INCLUDE_PS 1") != std::string::npos);
        MHF_CHECK(ps.find("u_smaa_metrics") != std::string::npos);
        MHF_CHECK(ps.find(p.ps_call) != std::string::npos);
        MHF_CHECK(ps.size() > 50000u);
    }

    return mhf::test::finish("psp_smaa");
}
