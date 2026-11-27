// Texture filtering policy (frontend/texture_filter.hpp): Auto / Linear / Nearest mode parsing and resolution rules.

#include "texture_filter.hpp"

#include "check.hpp"

using namespace mhf::psp;

int main() {
    // Round-trip name / parse and invalid input rejection.
    MHF_CHECK(texture_filter_name(TextureFilter::Auto) == "auto");
    MHF_CHECK(texture_filter_name(TextureFilter::Linear) == "linear");
    MHF_CHECK(texture_filter_name(TextureFilter::Nearest) == "nearest");

    MHF_CHECK(parse_texture_filter("auto") == TextureFilter::Auto);
    MHF_CHECK(parse_texture_filter("linear") == TextureFilter::Linear);
    MHF_CHECK(parse_texture_filter("nearest") == TextureFilter::Nearest);
    MHF_CHECK(!parse_texture_filter("").has_value());
    MHF_CHECK(!parse_texture_filter("AUTO").has_value());
    MHF_CHECK(!parse_texture_filter("bilinear").has_value());
    MHF_CHECK(!parse_texture_filter("trilinear").has_value());

    // Auto: respects the game's GE kTexFilter bit for both 3D and 2D draws.
    MHF_CHECK(!resolve_texture_linear({TextureFilter::Auto, false, false, false, false, false, 0}));
    MHF_CHECK(resolve_texture_linear({TextureFilter::Auto, true, false, false, false, false, 0}));
    MHF_CHECK(!resolve_texture_linear({TextureFilter::Auto, false, true, false, false, false, 0}));
    MHF_CHECK(resolve_texture_linear({TextureFilter::Auto, true, true, false, false, false, 0}));

    // Linear: forces bilinear on 3D (!through), leaves unresized 2D (through) as requested by the game.
    MHF_CHECK(resolve_texture_linear({TextureFilter::Linear, false, false, false, false, false, 0}));
    MHF_CHECK(resolve_texture_linear({TextureFilter::Linear, true, false, false, false, false, 0}));
    MHF_CHECK(!resolve_texture_linear({TextureFilter::Linear, false, true, false, false, false, 0}));
    MHF_CHECK(resolve_texture_linear({TextureFilter::Linear, true, true, false, false, false, 0}));

    // Nearest: forces nearest on regular 3D and 2D game textures even when the game requests linear.
    MHF_CHECK(!resolve_texture_linear({TextureFilter::Nearest, true, false, false, false, false, 0}));
    MHF_CHECK(!resolve_texture_linear({TextureFilter::Nearest, false, false, false, false, false, 0}));
    MHF_CHECK(!resolve_texture_linear({TextureFilter::Nearest, true, true, false, false, false, 0}));
    MHF_CHECK(!resolve_texture_linear({TextureFilter::Nearest, false, true, false, false, false, 0}));

    // Resized 2D (HUD scale < 100% or widescreen narrowing) stays linear in all modes.
    MHF_CHECK(resolve_texture_linear({TextureFilter::Auto, false, true, true, false, false, 0}));
    MHF_CHECK(resolve_texture_linear({TextureFilter::Linear, false, true, true, false, false, 0}));
    MHF_CHECK(resolve_texture_linear({TextureFilter::Nearest, false, true, true, false, false, 0}));

    // HD text preservation: stays linear in Nearest when game_linear or mipmapped; Auto still matches game_linear.
    MHF_CHECK(resolve_texture_linear({TextureFilter::Nearest, true, true, false, true, false, 0}));
    MHF_CHECK(resolve_texture_linear({TextureFilter::Nearest, false, true, false, true, true, 0}));
    MHF_CHECK(!resolve_texture_linear({TextureFilter::Nearest, false, true, false, true, false, 0}));
    MHF_CHECK(!resolve_texture_linear({TextureFilter::Auto, false, true, false, true, true, 0}));
    MHF_CHECK(resolve_texture_linear({TextureFilter::Auto, true, true, false, true, true, 0}));

    // Texture pack [filtering] override takes precedence over global mode, resized HUD, and HD text.
    MHF_CHECK(!resolve_texture_linear({TextureFilter::Linear, true, false, true, true, true, 1}));
    MHF_CHECK(!resolve_texture_linear({TextureFilter::Auto, true, true, false, false, false, 1}));
    MHF_CHECK(resolve_texture_linear({TextureFilter::Nearest, false, false, false, false, false, 2}));
    MHF_CHECK(resolve_texture_linear({TextureFilter::Nearest, false, true, false, false, false, 2}));
    // pack_filter == 0 defers to global mode.
    MHF_CHECK(!resolve_texture_linear({TextureFilter::Nearest, true, false, false, false, false, 0}));
    MHF_CHECK(resolve_texture_linear({TextureFilter::Linear, false, false, false, false, false, 0}));

    // Mipmap blending and anisotropy policy (set_texture_params):
    // Auto and Linear preserve main's GL_NEAREST_MIPMAP_LINEAR + anisotropy when a mipmapped pack texture resolves to nearest.
    {
        const TextureSampling auto_nearest = resolve_texture_sampling({TextureFilter::Auto, false, false, false, false, true, 0});
        MHF_CHECK(!auto_nearest.linear && auto_nearest.mip_linear_blend && auto_nearest.anisotropy);
        const TextureSampling auto_pack_nearest = resolve_texture_sampling({TextureFilter::Auto, true, false, false, false, true, 1});
        MHF_CHECK(!auto_pack_nearest.linear && auto_pack_nearest.mip_linear_blend && auto_pack_nearest.anisotropy);
        const TextureSampling auto_linear = resolve_texture_sampling({TextureFilter::Auto, true, false, false, false, true, 0});
        MHF_CHECK(auto_linear.linear && auto_linear.mip_linear_blend && auto_linear.anisotropy);
        const TextureSampling linear_2d_nearest = resolve_texture_sampling({TextureFilter::Linear, false, true, false, false, true, 0});
        MHF_CHECK(!linear_2d_nearest.linear && linear_2d_nearest.mip_linear_blend && linear_2d_nearest.anisotropy);

        // Explicit Nearest disables mipmap linear blending and anisotropy on draws that resolve to nearest,
        // while preserving linear mip blending and anisotropy when a pack [filtering] override or HD text stays linear.
        const TextureSampling forced_nearest = resolve_texture_sampling({TextureFilter::Nearest, true, false, false, false, true, 0});
        MHF_CHECK(!forced_nearest.linear && !forced_nearest.mip_linear_blend && !forced_nearest.anisotropy);
        const TextureSampling forced_nearest_pack = resolve_texture_sampling({TextureFilter::Nearest, true, false, false, false, true, 1});
        MHF_CHECK(!forced_nearest_pack.linear && !forced_nearest_pack.mip_linear_blend && !forced_nearest_pack.anisotropy);
        const TextureSampling nearest_pack_linear = resolve_texture_sampling({TextureFilter::Nearest, false, false, false, false, true, 2});
        MHF_CHECK(nearest_pack_linear.linear && nearest_pack_linear.mip_linear_blend && nearest_pack_linear.anisotropy);
        const TextureSampling nearest_hd_text = resolve_texture_sampling({TextureFilter::Nearest, false, true, false, true, true, 0});
        MHF_CHECK(nearest_hd_text.linear && nearest_hd_text.mip_linear_blend && nearest_hd_text.anisotropy);
    }

    return mhf::test::finish("psp_texture_filter");
}
