#include "check.hpp"
#include "gathering_art.hpp"
using namespace mhf::mhfu;
int main() {
    GatheringArt art;
    for (auto &image : art) {
        image.size = 2;
        image.pixels = std::make_shared<const std::vector<std::uint32_t>>(4, 0xffffffffu);
    }
    auto sprites = gathering_sprites(art, GatheringKind::mining, 255, true);
    MHF_CHECK(sprites.size() == 3);
    if (sprites.size() == 3) {
        MHF_CHECK(sprites[0].x == 225 && sprites[0].y == 80 && sprites[0].size == 32);
        MHF_CHECK(sprites[1].x == 229 && sprites[1].y == 84 && sprites[1].size == 24 &&
                  sprites[1].pixels == art[2].pixels);
        MHF_CHECK(sprites[2].x == 219 && sprites[2].y == 100 && sprites[2].size == 15 &&
                  sprites[2].pixels == art[1].pixels);
    }
    auto bugs = gathering_sprites(art, GatheringKind::bugs, 255, false);
    MHF_CHECK(bugs.size() == 2);
    if (bugs.size() == 2) MHF_CHECK(bugs[1].pixels == art[3].pixels);
    auto gather = gathering_sprites(art, GatheringKind::gather, 255, true);
    MHF_CHECK(gather.size() == 3);
    if (gather.size() == 3) MHF_CHECK(gather[1].pixels == art[4].pixels);
    auto blocked = gathering_sprites(art, GatheringKind::mining, 255, false, true);
    MHF_CHECK(blocked.size() == 3);
    if (blocked.size() == 3) {
        MHF_CHECK(blocked[2].x == 229 && blocked[2].y == 84 && blocked[2].size == 24);
        MHF_CHECK(blocked[2].pixels != art[1].pixels); // X replaces Circle
        bool red = false;
        for (auto pixel : *blocked[2].pixels)
            red |= (pixel & 255u) > 200 && ((pixel >> 8) & 255u) < 80 && (pixel >> 24) > 200;
        MHF_CHECK(red);
    }
    auto carve = gathering_sprites(art, GatheringKind::carve, 255, true);
    MHF_CHECK(carve.size() == 3 && carve[1].pixels == art[4].pixels);
    auto faded = gathering_sprites(art, GatheringKind::bugs, 15, true);
    MHF_CHECK(faded.size() == 1);
    if (faded.size() == 1) MHF_CHECK(faded[0].alpha == 15.0f / 255.0f);
    MHF_CHECK(gathering_sprites(art, GatheringKind::none, 0, false).empty());
    MHF_CHECK(gathering_alpha(0, true) == 15);
    MHF_CHECK(gathering_alpha(15, true) == 79);
    MHF_CHECK(gathering_alpha(207, true) == 255);
    MHF_CHECK(gathering_alpha(255, false) == 191);
    MHF_CHECK(gathering_alpha(63, false) == 15);
    return mhf::test::finish("test_gathering_art");
}
