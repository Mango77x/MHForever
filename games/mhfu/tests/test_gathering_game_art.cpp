#include "check.hpp"
#include "gathering_game_art.hpp"
#include <algorithm>
using namespace mhf::mhfu;
namespace {
void put(std::vector<std::uint8_t> &b, std::size_t o, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        b[o + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::vector<std::uint8_t> atlas(unsigned count) {
    constexpr unsigned image_size = 16 + 16 + 65536 + 16 + 1024;
    std::vector<std::uint8_t> b(16 + count * image_size);
    std::copy_n(".TMH0.14", 8, b.begin());
    put(b, 8, count);
    for (unsigned i = 0; i < count; ++i) {
        auto o = 16 + i * image_size;
        put(b, o, image_size);
        put(b, o + 8, 1);
        put(b, o + 12, 1);
        put(b, o + 16, 65552);
        put(b, o + 20, 1);
        put(b, o + 24, 5);
        put(b, o + 28, 0x01000100);
        for (unsigned y = 0; y < 256; ++y)
            for (unsigned x = 0; x < 256; ++x)
                b[o + 32 + (y / 8) * 2048 + (x / 16) * 128 + (y % 8) * 16 + x % 16] = static_cast<std::uint8_t>(x ^ y);
        auto p = o + 32 + 65536;
        put(b, p, 1040);
        put(b, p + 4, 2);
        put(b, p + 8, 3);
        put(b, p + 12, 256);
        for (unsigned k = 0; k < 256; ++k)
            put(b, p + 16 + 4 * k, 0x80004000u | k);
    }
    return b;
}
} // namespace
int main() {
    auto hud = atlas(2), items = atlas(1);
    auto art = decode_game_gathering_art(hud, items);
    MHF_CHECK(art.has_value());
    if (art) {
        constexpr int sizes[] = {32, 16, 24, 24, 24}, xs[] = {220, 0, 24, 144, 48}, ys[] = {224, 0, 48, 24, 96};
        for (unsigned i = 0; i < 5; ++i) {
            MHF_CHECK((*art)[i].size == sizes[i]);
            for (int y = 0; y < sizes[i]; ++y)
                for (int x = 0; x < sizes[i]; ++x) {
                    auto red = unsigned((xs[i] + x) ^ (ys[i] + y));
                    MHF_CHECK((*(*art)[i].pixels)[y * sizes[i] + x] == (0x80002000u | ((red * 128 + 127) / 255)));
                }
        }
    }
    MHF_CHECK(!decode_game_gathering_art({}, items));
    MHF_CHECK(!decode_game_gathering_art(hud, {}));
    for (auto n : {0u, 8u, 16u, 32u, 65560u})
        MHF_CHECK(!decode_game_gathering_art(std::span(hud).first(n), items));
    auto bad = hud;
    bad[0] = 0;
    MHF_CHECK(!decode_game_gathering_art(bad, items));
    bad = hud;
    put(bad, 8, 0xffffffff);
    MHF_CHECK(!decode_game_gathering_art(bad, items));
    bad = items;
    put(bad, 16 + 24, 4);
    MHF_CHECK(!decode_game_gathering_art(hud, bad));
    bad = items;
    put(bad, 16 + 28, 0xffff0100);
    MHF_CHECK(!decode_game_gathering_art(hud, bad));
    bad = items;
    put(bad, 16 + 12, 0);
    MHF_CHECK(!decode_game_gathering_art(hud, bad));
    bad = items;
    put(bad, 16 + 32 + 65536 + 8, 2);
    MHF_CHECK(!decode_game_gathering_art(hud, bad));
    MHF_CHECK(!load_game_gathering_art("missing.iso", "missing.elf", 18, 5323));
    return mhf::test::finish("test_gathering_game_art");
}
