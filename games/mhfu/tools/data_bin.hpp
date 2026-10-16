#pragma once

// Reader for the game's DATA.BIN container (docs/06-mhfu-module.md §4).
//
// DATA.BIN starts with a scrambled header: a table of first-block indices (one per file plus a sentinel equal to the
// total number of 2048-byte blocks) followed by (file_id, size) pairs for files whose size is not a whole number of
// blocks. Files are scrambled with the game's own byte-substitution table plus a keystream seeded by the file's first
// block. This is the game's data obfuscation, not Sony DRM; the game's own code undoes it at runtime.
//
// This module never embeds any data from the game. The 256-byte substitution table is located inside the user's own
// executable and validated against the user's own DATA.BIN. Developer-only tooling (CONTRIBUTING.md).

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace mhf::mhfu {

using SubstitutionTable = std::array<std::uint8_t, 256>;

// Keystream generator used by the game's file system.
class Keystream {
public:
    explicit Keystream(std::uint32_t seed) noexcept;
    std::uint32_t next() noexcept;

private:
    std::uint32_t low_;
    std::uint32_t high_;
};

// Unscrambles `data` in place (length must be a multiple of 4).
void unscramble(std::span<std::uint8_t> data, const SubstitutionTable& table, std::uint32_t seed);

// Inverse of unscramble(); used by tests to build synthetic containers.
void scramble(std::span<std::uint8_t> data, const SubstitutionTable& table, std::uint32_t seed);

// Every 4-byte-aligned window of `executable` whose 256 bytes are a permutation of 0..255.
std::vector<SubstitutionTable> find_substitution_table_candidates(std::span<const std::uint8_t> executable);

struct FileEntry {
    std::uint32_t id{};
    std::uint32_t first_block{};
    std::uint32_t size{};
};

class DataBin {
public:
    static constexpr std::uint32_t kBlockSize = 2048;

    // Reads `size` bytes at byte `offset` of DATA.BIN.
    using Reader = std::function<std::vector<std::uint8_t>(std::uint64_t offset, std::size_t size)>;

    // Tries each candidate table and keeps the one that yields a consistent header for a container of
    // `total_size` bytes. Returns std::nullopt if none does.
    static std::optional<DataBin> open(Reader reader, std::uint64_t total_size,
                                       const std::vector<SubstitutionTable>& candidates);

    [[nodiscard]] const std::vector<FileEntry>& files() const noexcept { return files_; }
    [[nodiscard]] const SubstitutionTable& table() const noexcept { return table_; }

    // Returns the unscrambled first `size` bytes of file `id` (all of it when size is std::nullopt).
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint32_t id, std::optional<std::uint32_t> size = std::nullopt) const;

private:
    DataBin(Reader reader, SubstitutionTable table, std::vector<FileEntry> files)
        : reader_(std::move(reader)), table_(table), files_(std::move(files)) {}

    Reader reader_;
    SubstitutionTable table_{};
    std::vector<FileEntry> files_;
};

// Metrowerks overlay header ("MWo3"), 64 bytes at the start of every code overlay.
struct OverlayHeader {
    std::uint32_t overlay_id{};
    std::uint32_t load_address{};
    std::uint32_t text_size{};
    std::uint32_t data_size{};
    std::uint32_t bss_size{};
    std::uint32_t static_init_start{};
    std::uint32_t static_init_end{};
    char name[33]{};

    static constexpr std::size_t kSize = 64;
    [[nodiscard]] std::uint32_t file_size() const noexcept { return static_cast<std::uint32_t>(kSize) + text_size + data_size; }
};

std::optional<OverlayHeader> parse_overlay_header(std::span<const std::uint8_t> bytes);

} // namespace mhf::mhfu
