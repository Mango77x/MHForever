#include "gathering_game_art.hpp"
#include "game_files.hpp"
#include "texture_images.hpp"
#include "texture_pack.hpp"

#include <algorithm>

namespace mhf::mhfu {
namespace {
using Bytes = std::span<const std::uint8_t>;
std::uint32_t word(Bytes bytes, std::size_t offset) {
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
           (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
}
std::uint32_t premultiply(std::uint32_t color) {
    const unsigned alpha = color >> 24;
    std::uint32_t pixel = alpha << 24;
    for (unsigned channel = 0; channel < 3; ++channel)
        pixel |= ((((color >> (channel * 8)) & 255u) * alpha + 127) / 255) << (channel * 8);
    return pixel;
}
struct Atlas {
    Bytes pixels, palette;
};
// Native TMH images are swizzled. The two atlases we use are 256x256 CLUT8 with a 256-entry RGBA8888 palette.
// Other images in the same container are skipped by their declared size, without decoding them.
std::optional<Atlas> atlas(Bytes bytes, unsigned index) {
    if (bytes.size() < 16 || !std::equal(bytes.begin(), bytes.begin() + 8, ".TMH0.14")) return {};
    const auto count = word(bytes, 8);
    if (count > 16 || index >= count) return {};
    std::size_t offset = 16;
    for (unsigned i = 0; i <= index; ++i) {
        if (offset > bytes.size() || bytes.size() - offset < 32) return {};
        const auto size = word(bytes, offset);
        if (size < 32 || size > bytes.size() - offset) return {};
        if (i == index) {
            if (word(bytes, offset + 12) != 1 || word(bytes, offset + 16) != 65552 || word(bytes, offset + 24) != 5 ||
                word(bytes, offset + 28) != 0x01000100 || size != 66608)
                return {};
            const auto palette = offset + 32 + 65536;
            if (word(bytes, palette) != 1040 || word(bytes, palette + 8) != 3 || word(bytes, palette + 12) != 256)
                return {};
            return Atlas{bytes.subspan(offset + 32, 65536), bytes.subspan(palette + 16, 1024)};
        }
        offset += size;
    }
    return {};
}
// Match the original atlas using the selected pack's own hash rules and palette, never a pack-specific filename.
// No live guest address is needed for content/palette keys. Address-only mappings keep the original atlas.
std::optional<psp::textures::Image> replacement(const Atlas &image, const psp::textures::Pack *pack) {
    if (!pack) return {};
    psp::textures::TextureFacts facts;
    facts.width = facts.height = facts.buffer_width = 256;
    facts.format = 5;
    facts.dimension = 0x0808;
    facts.clut_format_word = 0xc500ff03;
    facts.clut = image.palette.data();
    facts.clut_last_bytes = facts.clut_max_bytes = 1024;
    psp::textures::Key key;
    std::uint32_t width, height;
    if (!pack->key_of(
            facts,
            [&](std::uint32_t address, std::uint32_t size) {
                return address == 0 && size <= image.pixels.size() ? image.pixels.data() : nullptr;
            },
            key, width, height) ||
        width != 256 || height != 256)
        return {};
    const auto match = pack->find(key);
    if (!match.entry || match.entry->keeps_original()) return {};
    const auto level = match.entry->levels.find(0);
    if (level == match.entry->levels.end() || level->second.empty()) return {};
    psp::textures::Image decoded;
    std::string error;
    if (!psp::textures::decode_image(pack->path_of(level->second), decoded, error) || decoded.width < 256 ||
        decoded.width > 8192 || decoded.width != decoded.height || decoded.width % 256 != 0 || decoded.levels.empty() ||
        decoded.levels.front().size() != std::size_t(decoded.width) * decoded.height * 4)
        return {};
    return decoded;
}
GatheringImage crop(const psp::textures::Image &image, unsigned x, unsigned y, unsigned size) {
    const unsigned scale = image.width / 256, side = size * scale;
    auto pixels = std::make_shared<std::vector<std::uint32_t>>(side * side);
    const auto &rgba = image.levels.front();
    for (unsigned row = 0; row < side; ++row) {
        for (unsigned column = 0; column < side; ++column) {
            const auto offset = ((y * scale + row) * image.width + x * scale + column) * 4;
            (*pixels)[row * side + column] = premultiply(word(rgba, offset));
        }
    }
    return {static_cast<int>(side), std::move(pixels)};
}
GatheringImage crop(const Atlas &image, unsigned x, unsigned y, unsigned size) {
    auto pixels = std::make_shared<std::vector<std::uint32_t>>(size * size);
    for (unsigned row = 0; row < size; ++row) {
        for (unsigned column = 0; column < size; ++column) {
            const unsigned u = x + column, v = y + row;
            const unsigned swizzled = (v / 8) * 2048 + (u / 16) * 128 + (v % 8) * 16 + u % 16;
            (*pixels)[row * size + column] = premultiply(word(image.palette, 4 * image.pixels[swizzled]));
        }
    }
    return {static_cast<int>(size), std::move(pixels)};
}
} // namespace

std::optional<GatheringArt> decode_game_gathering_art(Bytes hud, Bytes items, const psp::textures::Pack *pack,
                                                      unsigned *replaced) {
    if (replaced) *replaced = 0;
    const auto background = atlas(hud, 1), actions = atlas(items, 0);
    if (!background || !actions) return {};
    const auto hud_image = replacement(*background, pack), item_image = replacement(*actions, pack);
    if (replaced) *replaced = unsigned(hud_image.has_value()) + unsigned(item_image.has_value());
    const auto hud_crop = [&](unsigned x, unsigned y, unsigned size) {
        return hud_image ? crop(*hud_image, x, y, size) : crop(*background, x, y, size);
    };
    const auto item_crop = [&](unsigned x, unsigned y) {
        return item_image ? crop(*item_image, x, y, 24) : crop(*actions, x, y, 24);
    };
    return GatheringArt{hud_crop(220, 224, 32), hud_crop(0, 0, 16), item_crop(24, 48), item_crop(144, 24),
                        item_crop(48, 96)};
}
std::optional<GatheringArt> load_game_gathering_art(const std::filesystem::path &iso_path,
                                                    const std::filesystem::path &executable, std::uint32_t hud_file,
                                                    std::uint32_t items_file, const psp::textures::Pack *pack,
                                                    unsigned *replaced) {
    if (replaced) *replaced = 0;
    // Use the existing game-specific DATA.BIN reader, with the player's own prepared executable and ISO.
    const std::uint32_t ids[] = {hud_file, items_file};
    const auto files = read_game_files(iso_path, executable, ids);
    if (!files) return {}; // Unsupported/missing input keeps the text fallback.
    return decode_game_gathering_art((*files)[0], (*files)[1], pack, replaced);
}
} // namespace mhf::mhfu
