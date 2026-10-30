// Texture packs in PPSSPP's format (a folder with a textures.ini and the images it names): reading the ini, choosing
// the one for the release being played, the keys a texture is known by, and finding a texture's replacement.
//
// The format follows PPSSPP's documentation and its texture replacer, whose rules are reproduced here: the key layout
// (address, dimensions and palette hash, then the data hash), the hashes (quick, xxh32, xxh64) and what they cover,
// the order in which wildcard keys are tried, and the [options], [hashes], [hashranges], [filtering],
// [reducehashranges] and [games] sections. The quick hash and the byte range it covers were first written for
// MHForever by ibonplix; the design of loading packs (one key per upload, images decoded away from the game, a memory
// budget, a review before installing) follows Yakumo's (TeamGDB/Yakumo, MIT). See THIRD_PARTY.md.
//
// Nothing here draws or decodes images (texture_images.hpp does): the launcher uses it to review a pack.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mhf::psp::textures {

enum class HashKind { Quick, Xxh32, Xxh64 };

struct Options {
    HashKind hash = HashKind::Quick;
    bool ignore_address = false; // keys without the texture's address (not with quick: PPSSPP turns it off)
    bool reduce_hash = false;    // hash only part of each texture (not with quick either)
    float reduce_hash_default = 0.5f;
    std::uint32_t skip_last_dxt1_blocks_128x64 = 0, skip_last_dxt1_blocks_128x128 = 0;
    int version = 1;
};

// A texture's key: the cache part (address << 32, dimensions, palette hash) and the hash of its data. Printed as 16 + 8
// hex digits, the way packs name textures.
struct Key {
    std::uint64_t cache = 0;
    std::uint32_t data = 0;
    bool operator==(const Key &) const = default;
};
std::string format_key(const Key &key);

// What the GE knows of the current texture, level 0.
struct TextureFacts {
    std::uint32_t address = 0;      // guest address
    std::uint32_t buffer_width = 0; // the texture buffer width register (texels)
    std::uint32_t format = 0;       // 0..10: 5650, 5551, 4444, 8888, CLUT4, CLUT8, CLUT16, CLUT32, DXT1, DXT3, DXT5
    std::uint32_t width = 0, height = 0;
    std::uint16_t dimension = 0;    // log2 width | log2 height << 8, as the texture size register has them
    std::uint16_t max_seen_v = 0;   // the lowest row drawn so far in through mode (0: unknown)
    // The palette, for CLUT formats: the CLUT format command word (command byte included), the palette's bytes as the
    // loads left them, how many the last load wrote and the most any load wrote.
    std::uint32_t clut_format_word = 0;
    const std::uint8_t *clut = nullptr;
    std::uint32_t clut_last_bytes = 0, clut_max_bytes = 0;
};
// Guest memory as the host sees it: `size` bytes at `address`, or nullptr when they are not all mapped.
using MemoryReader = std::function<const std::uint8_t *(std::uint32_t address, std::uint32_t size)>;

// PPSSPP's quick hash of `size` bytes (`aligned16`: the data starts on a 16-byte boundary).
std::uint32_t quick_hash(const std::uint8_t *data, std::uint32_t size, bool aligned16);

enum class Filter { Auto, Nearest, Linear };

// What a pack says of one texture: the image of each mip level (an empty name keeps the game's own texture).
struct Entry {
    std::map<int, std::string> levels;
    [[nodiscard]] const std::string &image() const { return levels.begin()->second; }
    [[nodiscard]] bool keeps_original() const { return levels.empty() || levels.begin()->second.empty(); }
};

// What a lookup found: the entry (nullptr: not in the pack), the filter the pack asks for, and how much of the texture
// the image covers (a [hashranges] entry makes it a part of it).
struct Match {
    const Entry *entry = nullptr;
    Filter filter = Filter::Auto;
    std::uint32_t covered_width = 0, covered_height = 0;
};

// What reviewing a pack tells (the launcher shows it before the pack is used).
struct Review {
    std::filesystem::path folder; // the folder holding the ini
    std::filesystem::path ini;    // the ini chosen for the release
    std::string hash;             // "quick", "xxh32" or "xxh64"
    std::size_t keys = 0;         // textures the ini names
    std::size_t images = 0;       // distinct image files it names
    std::size_t missing = 0;      // of those, not in the folder
    std::uint64_t bytes = 0;      // size of the images found
    std::vector<std::string> games; // [games], when the ini has one
    std::vector<std::string> warnings;
};

class Pack {
public:
    // Opens the pack in `folder` for the release `game_id` ("ULES01213"; empty: any). The ini is the one whose [games]
    // lists the release, else one named for its region (textures_FU_EU.ini, *_US*, *_JP*), else textures.ini. Returns
    // nullptr with `error` set when there is none, it cannot be read, or its [games] names only other titles.
    static std::unique_ptr<Pack> open(const std::filesystem::path &folder, const std::string &game_id, std::string &error);

    // The folder holding a pack inside `chosen`: itself, or a single sub-folder that holds an ini (a pack's MAIN), or
    // PPSSPP's PSP/TEXTURES/<game_id>. Empty when there is none.
    static std::filesystem::path locate(const std::filesystem::path &chosen, const std::string &game_id);

    // Opens the pack and checks its images exist (and their total size). False with `error` as open() would.
    static bool review(const std::filesystem::path &folder, const std::string &game_id, Review &review, std::string &error);

    // A pack with no images, only to make keys with `options` (texture dumps without a pack).
    static std::unique_ptr<Pack> keys_only(const Options &options = {});

    [[nodiscard]] const Options &options() const noexcept { return options_; }
    [[nodiscard]] const std::filesystem::path &folder() const noexcept { return folder_; }
    [[nodiscard]] const std::filesystem::path &ini() const noexcept { return ini_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] const std::vector<std::string> &warnings() const noexcept { return warnings_; }

    // The key of the current texture, as the pack's options make it. False when its data cannot be read. `covered_*`
    // is the part of the texture the key covers (all of it, unless [hashranges] says otherwise).
    bool key_of(const TextureFacts &texture, const MemoryReader &memory, Key &key, std::uint32_t &covered_width,
                std::uint32_t &covered_height) const;
    // The entry for a key, trying the wildcard forms PPSSPP tries (no address, no palette, no data hash...).
    [[nodiscard]] Match find(const Key &key) const;
    // An image file of the pack: the path to read (the name as the ini spells it, or found again in another case).
    [[nodiscard]] std::filesystem::path path_of(const std::string &image) const;

private:
    struct KeyHash {
        std::size_t operator()(const Key &k) const noexcept { return std::hash<std::uint64_t>()(k.cache ^ (std::uint64_t(k.data) * 0x9E3779B97F4A7C15ull)); }
    };
    template <typename T>
    const T *lookup(const std::unordered_map<Key, T, KeyHash> &map, const Key &key) const;
    bool parse(const std::filesystem::path &ini, std::string &error);
    void add_named_images();

    std::filesystem::path folder_, ini_;
    Options options_;
    std::unordered_map<Key, Entry, KeyHash> entries_;
    std::unordered_map<Key, Filter, KeyHash> filters_;
    std::unordered_map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> hash_ranges_; // addr << 32 | w << 16 | h
    std::unordered_map<std::uint32_t, float> reduce_ranges_;                                   // w << 16 | h
    std::vector<std::string> games_, warnings_;
    // Lower-case relative path -> as on disk, built the first time a name is not found as spelled (Linux is
    // case-sensitive; packs are made on Windows). Loader threads ask concurrently.
    mutable std::unordered_map<std::string, std::filesystem::path> folded_names_;
    mutable std::once_flag folded_once_;
};

// Paths and UTF-8 text (packs name their images in UTF-8; C++20 spells that std::u8string).
std::string utf8(const std::filesystem::path &path);
std::filesystem::path from_utf8(const std::string &text);

// The game ids an ini's [games] lists, read without opening the pack. Empty when there is no such section.
std::vector<std::string> games_of(const std::filesystem::path &ini);

} // namespace mhf::psp::textures
