#include "gathering_game_art.hpp"
#include "data_bin.hpp"
#include "iso9660.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace mhf::mhfu {
namespace {
using Bytes = std::span<const std::uint8_t>;
std::uint32_t word(Bytes bytes, std::size_t offset) {
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
           (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
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
GatheringImage crop(const Atlas &image, unsigned x, unsigned y, unsigned size) {
    auto pixels = std::make_shared<std::vector<std::uint32_t>>(size * size);
    for (unsigned row = 0; row < size; ++row) {
        for (unsigned column = 0; column < size; ++column) {
            const unsigned u = x + column, v = y + row;
            const unsigned swizzled = (v / 8) * 2048 + (u / 16) * 128 + (v % 8) * 16 + u % 16;
            const auto color = word(image.palette, 4 * image.pixels[swizzled]);
            const unsigned alpha = color >> 24;
            std::uint32_t pixel = alpha << 24;
            for (unsigned channel = 0; channel < 3; ++channel)
                pixel |= ((((color >> (channel * 8)) & 255u) * alpha + 127) / 255) << (channel * 8);
            (*pixels)[row * size + column] = pixel;
        }
    }
    return {static_cast<int>(size), std::move(pixels)};
}
} // namespace

std::optional<GatheringArt> decode_game_gathering_art(Bytes hud, Bytes items) {
    const auto background = atlas(hud, 1), actions = atlas(items, 0);
    if (!background || !actions) return {};
    return GatheringArt{crop(*background, 220, 224, 32), crop(*background, 0, 0, 16), crop(*actions, 24, 48, 24),
                        crop(*actions, 144, 24, 24), crop(*actions, 48, 96, 24)};
}
std::optional<GatheringArt> load_game_gathering_art(const std::filesystem::path &iso_path,
                                                    const std::filesystem::path &executable, std::uint32_t hud_file,
                                                    std::uint32_t items_file) {
    try {
        // Use the existing game-specific DATA.BIN reader, with the player's own prepared executable and ISO.
        std::ifstream file(executable, std::ios::binary | std::ios::ate);
        if (!file || file.tellg() <= 0 || file.tellg() > 16 * 1024 * 1024) return {};
        std::vector<std::uint8_t> exe(static_cast<std::size_t>(file.tellg()));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char *>(exe.data()), static_cast<std::streamsize>(exe.size()))) return {};
        psp::IsoImage iso(iso_path);
        const auto entry = iso.find("/PSP_GAME/USRDIR/DATA.BIN");
        if (!entry) return {};
        const auto data = DataBin::open(
            [&](std::uint64_t offset, std::size_t size) {
                if (size > 1024 * 1024) throw std::runtime_error("icon archive read exceeds limit");
                return iso.read_file(*entry, offset, size);
            },
            entry->size, find_substitution_table_candidates(exe));
        if (!data) return {};
        for (const auto id : {hud_file, items_file}) {
            if (id >= data->files().size() || data->files()[id].size > 1024 * 1024) return {};
        }
        return decode_game_gathering_art(data->read(hud_file), data->read(items_file));
    } catch (const std::exception &) {
        return {}; // Unsupported/missing input keeps the text fallback.
    }
}
} // namespace mhf::mhfu
