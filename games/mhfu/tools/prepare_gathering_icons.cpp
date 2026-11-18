// Extracts optional gathering sprites from the player's own decoded/replacement atlases. No artwork is bundled.
#include "texture_images.hpp"

#include <array>
#include <cstdio>
#include <fstream>

namespace {
using mhf::psp::textures::Image;

bool load(const char *path, Image &image) {
    std::string error;
    if (!mhf::psp::textures::decode_image(path, image, error)) {
        std::fprintf(stderr, "%s: %s\n", path, error.c_str());
        return false;
    }
    if (image.width != image.height || image.width == 0 || image.width > 8192 || image.width % 256 != 0 ||
        image.levels.empty() || image.levels.front().size() != std::size_t(image.width) * image.height * 4) {
        std::fprintf(stderr, "%s: expected a square RGBA atlas on a 256-unit grid\n", path);
        return false;
    }
    return true;
}

bool crop(const Image &atlas, int x, int y, int size, const std::filesystem::path &path) {
    const unsigned scale = atlas.width / 256, side = unsigned(size) * scale;
    if (side > 1024) return false; // matches the runtime loader's bound
    std::ofstream file(path, std::ios::binary);
    file.write("MHFICON1", 8);
    const std::array<unsigned char, 4> header{static_cast<unsigned char>(side), static_cast<unsigned char>(side >> 8),
                                              static_cast<unsigned char>(side >> 16),
                                              static_cast<unsigned char>(side >> 24)};
    file.write(reinterpret_cast<const char *>(header.data()), header.size());
    const auto &pixels = atlas.levels.front();
    for (unsigned row = 0; row < side; ++row) {
        for (unsigned column = 0; column < side; ++column) {
            const auto offset = ((unsigned(y) * scale + row) * atlas.width + unsigned(x) * scale + column) * 4;
            std::array<unsigned char, 4> rgba{};
            const unsigned alpha = pixels[offset + 3];
            for (unsigned channel = 0; channel < 3; ++channel)
                rgba[channel] = static_cast<unsigned char>((pixels[offset + channel] * alpha + 127) / 255);
            rgba[3] = static_cast<unsigned char>(alpha);
            file.write(reinterpret_cast<const char *>(rgba.data()), rgba.size());
        }
    }
    return bool(file);
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: mhfu_prepare_gathering_icons <hud.ktx2|png> <items.ktx2|png> <output-folder>\n");
        return 2;
    }
    Image hud, items;
    if (!load(argv[1], hud) || !load(argv[2], items)) return 1;
    const std::filesystem::path output = argv[3];
    std::error_code error;
    std::filesystem::create_directories(output, error);
    if (error) {
        std::fprintf(stderr, "%s\n", error.message().c_str());
        return 1;
    }
    // Original mod coordinates on the 256x256 HUD/items atlases, scaled for replacement textures.
    const bool ok = crop(hud, 220, 224, 32, output / "background.rgba") &&
                    crop(hud, 0, 0, 16, output / "circle.rgba") && crop(items, 24, 48, 24, output / "mining.rgba") &&
                    crop(items, 144, 24, 24, output / "bugs.rgba") && crop(items, 48, 96, 24, output / "gather.rgba");
    if (!ok) std::fprintf(stderr, "could not write gathering icons\n");
    return ok ? 0 : 1;
}
