// SMAA shader assembly and LUT integrity checks (frontend/smaa_shaders.*): runs without OpenGL or game data.
// Pass `--dump-shaders <dir>` to write all 6 assembled GLSL 330 stages for external validation (e.g. glslangValidator -l).

#include "smaa_shaders.hpp"

#include "check.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace mhf::psp::smaa;

namespace {

bool has_line_continuation(std::string_view src) {
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i] != '\\') continue;
        std::size_t j = i + 1;
        while (j < src.size() && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r')) ++j;
        if (j == src.size() || src[j] == '\n') return true;
    }
    return false;
}

bool write_stage(const std::filesystem::path &path, const std::string &src) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(src.data(), static_cast<std::streamsize>(src.size()));
    return static_cast<bool>(out);
}

} // namespace

int main(int argc, char **argv) {
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

    std::filesystem::path dump_dir;
    if (argc == 3 && std::string_view(argv[1]) == "--dump-shaders") {
        dump_dir = argv[2];
        std::filesystem::create_directories(dump_dir);
    }

    // Every pass produces strict GLSL 330 Core vertex and fragment shaders wrapping embedded SMAA.hlsl.
    const struct {
        Pass pass;
        const char *name;
        const char *vs_call;
        const char *ps_call;
    } passes[] = {
        {Pass::EdgeDetection, "edge", "SMAAEdgeDetectionVS(", "SMAAColorEdgeDetectionPS("},
        {Pass::BlendingWeight, "weight", "SMAABlendingWeightCalculationVS(", "SMAABlendingWeightCalculationPS("},
        {Pass::NeighborhoodBlending, "blend", "SMAANeighborhoodBlendingVS(", "SMAANeighborhoodBlendingPS("},
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
        MHF_CHECK(vs.size() > 25000u); // embeds SMAA.hlsl
        MHF_CHECK(vs.find('\r') == std::string::npos);
        MHF_CHECK(!has_line_continuation(vs)); // GLSL 330 forbids '\' line continuations

        MHF_CHECK(ps.starts_with("#version 330 core\n"));
        MHF_CHECK(ps.find("#define SMAA_GLSL_3 1") != std::string::npos);
        MHF_CHECK(ps.find("#define SMAA_PRESET_HIGH 1") != std::string::npos);
        MHF_CHECK(ps.find("#define SMAA_INCLUDE_VS 0") != std::string::npos);
        MHF_CHECK(ps.find("#define SMAA_INCLUDE_PS 1") != std::string::npos);
        MHF_CHECK(ps.find("u_smaa_metrics") != std::string::npos);
        MHF_CHECK(ps.find(p.ps_call) != std::string::npos);
        MHF_CHECK(ps.size() > 25000u);
        MHF_CHECK(ps.find('\r') == std::string::npos);
        MHF_CHECK(!has_line_continuation(ps));

        if (!dump_dir.empty()) {
            MHF_CHECK(write_stage(dump_dir / (std::string(p.name) + ".vert"), vs));
            MHF_CHECK(write_stage(dump_dir / (std::string(p.name) + ".frag"), ps));
        }
    }

    return mhf::test::finish("psp_smaa");
}
