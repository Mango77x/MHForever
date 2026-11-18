#include "check.hpp"
#include "gathering_game_art.hpp"
#include "texture_images.hpp"
#include "texture_pack.hpp"
#include <algorithm>
#include <fstream>
#include <random>
#include <tuple>
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
    // Two packs use arbitrary filenames and different image scales: lookup follows each selected ini.
    namespace tx = mhf::psp::textures;
    const auto dir =
        std::filesystem::temp_directory_path() / ("gathering-packs-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    const auto key = [&](const std::vector<std::uint8_t> &tmh, unsigned image) {
        const std::size_t offset = 16 + image * (16 + 16 + 65536 + 16 + 1024);
        tx::TextureFacts f;
        f.buffer_width = f.width = f.height = 256;
        f.format = 5;
        f.dimension = 0x0808;
        f.clut_format_word = 0xc500ff03;
        f.clut = tmh.data() + offset + 32 + 65536 + 16;
        f.clut_last_bytes = f.clut_max_bytes = 1024;
        tx::Key k;
        std::uint32_t w, h;
        auto keys = tx::Pack::keys_only();
        MHF_CHECK(keys->key_of(
            f,
            [&](std::uint32_t address, std::uint32_t length) {
                return address == 0 && length <= 65536 ? tmh.data() + offset + 32 : nullptr;
            },
            k, w, h));
        k.cache = 0;
        return tx::format_key(k);
    };
    // Give the item atlas a different identity from the HUD.
    items[48] ^= 1;
    const auto baseline = decode_game_gathering_art(hud, items);
    MHF_CHECK(baseline.has_value());
    if (!baseline) return mhf::test::finish("test_gathering_game_art");
    const auto hud_key = key(hud, 1), item_key = key(items, 0);
    MHF_CHECK(hud_key != item_key);
    const auto make_pack = [&](const char *name, unsigned scale, std::uint32_t color) {
        auto folder = dir / name;
        std::filesystem::create_directory(folder);
        std::vector<std::uint32_t> pixels(256 * scale * 256 * scale);
        for (unsigned y = 0; y < 256 * scale; ++y)
            for (unsigned x = 0; x < 256 * scale; ++x)
                pixels[y * 256 * scale + x] =
                    (color & 0xffff0000u) | (((((color >> 8) & 255u) + y) & 255u) << 8) | (((color & 255u) + x) & 255u);
        MHF_CHECK(tx::write_png(folder / "arbitrary-atlas.png", 256 * scale, 256 * scale, pixels.data()));
        std::ofstream ini(folder / "textures.ini");
        ini << "[games]\nULES01213=true\n[hashes]\n"
            << hud_key << "=arbitrary-atlas.png\n"
            << item_key << "=arbitrary-atlas.png\n";
        return folder;
    };
    std::string error;
    auto first = make_pack("first", 1, 0x800040c8u), second = make_pack("second", 2, 0xffa06020u);
    for (const auto &[folder, scale, color] :
         {std::tuple{first, 1u, 0x800040c8u}, std::tuple{second, 2u, 0xffa06020u}}) {
        auto pack = tx::Pack::open(folder, "ULES01213", error);
        MHF_CHECK(pack != nullptr);
        unsigned replaced = 0;
        auto packed = decode_game_gathering_art(hud, items, pack.get(), &replaced);
        MHF_CHECK(packed.has_value() && replaced == 2);
        if (packed)
            for (unsigned i = 0; i < 5; ++i) {
                MHF_CHECK((*packed)[i].size == (*baseline)[i].size * int(scale));
                constexpr unsigned xs[] = {220, 0, 24, 144, 48}, ys[] = {224, 0, 48, 24, 96};
                for (unsigned offset : {0u, unsigned((*packed)[i].size - 1)}) {
                    const auto x = xs[i] * scale + offset, y = ys[i] * scale + offset;
                    const auto alpha = color >> 24;
                    const auto red = (((color & 255u) + x) & 255u) * alpha;
                    const auto green = ((((color >> 8) & 255u) + y) & 255u) * alpha;
                    const auto blue = ((color >> 16) & 255u) * alpha;
                    const auto expected = (alpha << 24) | (((blue + 127) / 255) << 16) | (((green + 127) / 255) << 8) |
                                          ((red + 127) / 255);
                    MHF_CHECK((*(*packed)[i].pixels)[offset * (*packed)[i].size + offset] == expected);
                }
            }
    }
    // Missing/invalid replacement images retain native icons, independently for each atlas.
    std::ofstream(first / "textures.ini") << "[hashes]\n"
                                          << hud_key << "=arbitrary-atlas.png\n"
                                          << item_key << "=missing.png\n";
    auto partial = tx::Pack::open(first, "ULES01213", error);
    unsigned replaced = 0;
    auto mixed = decode_game_gathering_art(hud, items, partial.get(), &replaced);
    MHF_CHECK(mixed.has_value() && replaced == 1);
    if (mixed) MHF_CHECK(*(*mixed)[2].pixels == *(*baseline)[2].pixels);
    std::ofstream(first / "arbitrary-atlas.png") << "not an image";
    replaced = 0;
    auto fallback = decode_game_gathering_art(hud, items, partial.get(), &replaced);
    MHF_CHECK(fallback.has_value() && replaced == 0);
    if (fallback)
        for (unsigned i = 0; i < 5; ++i)
            MHF_CHECK(*(*fallback)[i].pixels == *(*baseline)[i].pixels);
    std::vector<std::uint32_t> wrong_shape(256 * 128, 0xffffffffu);
    MHF_CHECK(tx::write_png(first / "arbitrary-atlas.png", 256, 128, wrong_shape.data()));
    replaced = 0;
    fallback = decode_game_gathering_art(hud, items, partial.get(), &replaced);
    MHF_CHECK(fallback.has_value() && replaced == 0);
    std::ofstream(first / "textures.ini") << "[hashes]\n" << hud_key << "=\n" << item_key << "=\n";
    auto originals = tx::Pack::open(first, "ULES01213", error);
    replaced = 0;
    fallback = decode_game_gathering_art(hud, items, originals.get(), &replaced);
    MHF_CHECK(fallback.has_value() && replaced == 0);
    std::filesystem::remove_all(dir);
    MHF_CHECK(!load_game_gathering_art("missing.iso", "missing.elf", 18, 5323));
    return mhf::test::finish("test_gathering_game_art");
}
