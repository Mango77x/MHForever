#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace mhf::psp::frontend {

// Texture pack in PPSSPP layout: the ini maps a 32-bit data hash to a file relative to the pack folder.
// The files are KTX2 (Basis Universal, Zstd); they are transcoded to RGBA8 on demand.
class TextureReplacer {
public:
    struct Image {
        std::uint32_t width = 0, height = 0;
        std::vector<std::uint8_t> rgba; // empty = could not be loaded
    };

    bool load(const std::filesystem::path &dir, const std::string &ini_name = "textures_FU_EU.ini");
    [[nodiscard]] const std::filesystem::path *find(std::uint32_t hash) const {
        const auto it = files_.find(hash);
        return it == files_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] std::size_t size() const noexcept { return files_.size(); }

    // Decoded replacement for a hash, or nullptr (not in the pack, or the file failed to load). The pointer is valid until
    // the next call. Decoded images are cached up to a memory limit.
    [[nodiscard]] const Image *image(std::uint32_t hash);

private:
    std::filesystem::path dir_;
    std::unordered_map<std::uint32_t, std::filesystem::path> files_;
    std::unordered_map<std::uint32_t, Image> images_;
    std::size_t cache_bytes_ = 0;
};

} // namespace mhf::psp::frontend
