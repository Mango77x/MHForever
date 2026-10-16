#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace mhf::psp {

// Minimal read-only ISO9660 reader for PSP UMD images (2048-byte sectors, no Joliet/UDF).
class IsoImage {
public:
    struct Entry {
        std::string path;   // absolute, uppercase as stored, without ";1" (e.g. "/PSP_GAME/USRDIR/DATA.BIN")
        std::uint32_t lba{};
        std::uint32_t size{};
        bool is_directory{};
    };

    static constexpr std::uint32_t kSectorSize = 2048;

    // Opens the image and indexes every file. Throws std::runtime_error on malformed images.
    explicit IsoImage(const std::filesystem::path& path);

    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

    // Case-insensitive lookup of a file by absolute path.
    [[nodiscard]] std::optional<Entry> find(std::string_view path) const;

    // Reads `size` bytes starting at byte `offset` from the beginning of the image.
    [[nodiscard]] std::vector<std::uint8_t> read_bytes(std::uint64_t offset, std::size_t size);

    // Reads `size` bytes at `offset` inside a file entry. The range must be within the file.
    [[nodiscard]] std::vector<std::uint8_t> read_file(const Entry& entry, std::uint64_t offset, std::size_t size);

private:
    void walk_directory(std::uint32_t lba, std::uint32_t size, const std::string& prefix, int depth);

    std::ifstream file_;
    std::uint64_t image_size_{};
    std::vector<Entry> entries_;
};

} // namespace mhf::psp
