#include "button_prompt_atlases.hpp"
#include "game_files.hpp"

#include <algorithm>
#include <array>

namespace mhf::mhfu {
namespace {
using Bytes = std::span<const std::uint8_t>;
using psp::prompts::Atlas;
using psp::prompts::Box;
using psp::prompts::Cell;
using psp::prompts::Icon;

std::uint32_t word(Bytes bytes, std::size_t offset) {
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
           (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
}

constexpr std::uint32_t kSide = 256, kTexels = kSide * kSide;

// Image `index` of a TMH container when it is a swizzled 256 x 256 one of 8-bit texels with a palette of 256 colours
// of 32 bits, as both atlases are: its texels as the game stores them, and its palette.
struct Image {
    Bytes texels, palette;
    [[nodiscard]] std::uint32_t colour(unsigned x, unsigned y) const {
        const unsigned swizzled = (y / 8) * 2048 + (x / 16) * 128 + (y % 8) * 16 + x % 16;
        return word(palette, 4u * texels[swizzled]);
    }
};

std::optional<Image> image(Bytes bytes, unsigned index) {
    static constexpr char kMagic[] = ".TMH0.14";
    if (bytes.size() < 16 || !std::equal(bytes.begin(), bytes.begin() + 8, kMagic)) return {};
    const auto count = word(bytes, 8);
    if (count > 16 || index >= count) return {};
    std::size_t offset = 16;
    for (unsigned i = 0; i <= index; ++i) {
        if (offset > bytes.size() || bytes.size() - offset < 32) return {};
        const auto size = word(bytes, offset);
        if (size < 32 || size > bytes.size() - offset) return {};
        if (i == index) {
            if (word(bytes, offset + 12) != 1 || word(bytes, offset + 16) != kTexels + 16 || word(bytes, offset + 24) != 5 ||
                word(bytes, offset + 28) != 0x01000100 || size != 32 + kTexels + 16 + 1024)
                return {};
            const auto palette = offset + 32 + kTexels;
            if (word(bytes, palette) != 1040 || word(bytes, palette + 8) != 3 || word(bytes, palette + 12) != 256) return {};
            return Image{bytes.subspan(offset + 32, kTexels), bytes.subspan(palette + 16, 1024)};
        }
        offset += size;
    }
    return {};
}

// The pieces are one picture with different marks on it: half of their texels or more are the same in all of them (in
// all but one, for four pieces), a good part of the piece is drawn on, and no two pieces are the same.
bool one_picture(const Image &image, std::span<const Box> pieces) {
    const unsigned w = pieces[0].w, h = pieces[0].h;
    const auto count = static_cast<std::ptrdiff_t>(pieces.size());
    unsigned shared = 0, drawn = 0;
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            std::array<std::uint32_t, 4> texels{};
            for (std::size_t i = 0; i < pieces.size(); ++i) texels[i] = image.colour(pieces[i].x + x, pieces[i].y + y);
            std::ptrdiff_t most = 0;
            for (std::size_t i = 0; i < pieces.size(); ++i) {
                most = std::max(most, std::count(texels.begin(), texels.begin() + count, texels[i]));
            }
            if (count > 2 ? most + 1 >= count : most == count) ++shared;
            if ((texels[0] >> 24) >= 128u) ++drawn;
        }
    }
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        for (std::size_t j = i + 1; j < pieces.size(); ++j) {
            bool same = true;
            for (unsigned y = 0; y < h && same; ++y) {
                for (unsigned x = 0; x < w && same; ++x) {
                    same = image.colour(pieces[i].x + x, pieces[i].y + y) == image.colour(pieces[j].x + x, pieces[j].y + y);
                }
            }
            if (same) return false;
        }
    }
    return shared * 2 >= w * h && drawn * 4 >= w * h;
}

// Where the pieces are: the face buttons side by side from (x, y), then L and R, 16 texels tall; `start` (the HUD's
// atlas only): the row of START and SELECT under them.
std::optional<Atlas> atlas(Bytes file, unsigned index, unsigned x, unsigned y, bool start) {
    const auto found = image(file, index);
    if (!found) return {};
    const auto at = [&](unsigned dx, unsigned dy, unsigned w, unsigned h) {
        return Box{static_cast<std::uint16_t>(x + dx), static_cast<std::uint16_t>(y + dy), static_cast<std::uint16_t>(w),
                   static_cast<std::uint16_t>(h)};
    };
    const Box faces[4] = {at(0, 0, 16, 16), at(16, 0, 16, 16), at(32, 0, 16, 16), at(48, 0, 16, 16)};
    if (!one_picture(*found, faces)) return {};
    Atlas out;
    out.width = out.height = kSide;
    out.cells = {{Icon::Circle, faces[0], {}, 0}, {Icon::Cross, faces[1], {}, 0}, {Icon::Triangle, faces[2], {}, 0},
                 {Icon::Square, faces[3], {}, 0}};
    // The plates: the box their lettering is in, and a row of the plate, under or over it, without any.
    const Box shoulders[2] = {at(64, 0, 20, 16), at(84, 0, 20, 16)};
    if (one_picture(*found, shoulders)) {
        out.cells.push_back({Icon::L, shoulders[0], at(69, 4, 11, 8), static_cast<std::uint16_t>(y + 12)});
        out.cells.push_back({Icon::R, shoulders[1], at(89, 4, 11, 8), static_cast<std::uint16_t>(y + 12)});
    }
    const Box plates[2] = {at(0, 16, 20, 16), at(20, 16, 20, 16)};
    if (start && one_picture(*found, plates)) {
        out.cells.push_back({Icon::Start, plates[0], {}, 0});
        out.cells.push_back({Icon::Select, plates[1], at(21, 21, 18, 5), static_cast<std::uint16_t>(y + 20)});
    }
    // The texture is told by the rows its pieces are in, as the game stores them (swizzled: 2048 bytes for 8 rows).
    out.mark_offset = (y / 8u) * 2048u;
    out.mark_size = (start ? 4u : 2u) * 2048u;
    out.mark = psp::prompts::mark_of(found->texels.subspan(out.mark_offset, out.mark_size));
    return out;
}
} // namespace

std::vector<Atlas> prompt_atlases(Bytes hud_file, Bytes menu_file) {
    std::vector<Atlas> out;
    if (auto hud = atlas(hud_file, 1, 0, 0, true)) out.push_back(std::move(*hud));
    if (auto menu = atlas(menu_file, 3, 104, 144, false)) out.push_back(std::move(*menu));
    return out;
}

std::vector<Atlas> load_prompt_atlases(const std::filesystem::path &iso, const std::filesystem::path &executable,
                                       std::optional<std::uint32_t> hud_file, std::optional<std::uint32_t> menu_file) {
    std::vector<std::uint32_t> ids;
    if (hud_file) ids.push_back(*hud_file);
    if (menu_file) ids.push_back(*menu_file);
    if (ids.empty()) return {};
    const auto files = read_game_files(iso, executable, ids);
    if (!files) return {};
    const Bytes none;
    return prompt_atlases(hud_file ? Bytes{files->front()} : none, menu_file ? Bytes{files->back()} : none);
}
} // namespace mhf::mhfu
