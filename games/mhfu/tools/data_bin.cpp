#include "data_bin.hpp"

#include <cstring>
#include <map>
#include <stdexcept>

namespace mhf::mhfu {

namespace {

std::uint32_t load_le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void store_le32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

} // namespace

Keystream::Keystream(std::uint32_t seed) noexcept
    : low_(seed & 0xFFFFu), high_((seed >> 16) & 0xFFFFu) {
    if (low_ == 0) {
        low_ = 0x7F8D;
    }
    if (high_ == 0) {
        high_ = 0x2345;
    }
}

std::uint32_t Keystream::next() noexcept {
    low_ = (low_ * 0x7F8Du) % 0xFFF1u;
    high_ = (high_ * 0x2345u) % 0xFFD9u;
    return (high_ << 16) + low_;
}

void unscramble(std::span<std::uint8_t> data, const SubstitutionTable& table, std::uint32_t seed) {
    if (data.size() % 4 != 0) {
        throw std::invalid_argument("unscramble: size must be a multiple of 4");
    }
    Keystream keys{seed};
    for (std::size_t i = 0; i < data.size(); i += 4) {
        for (std::size_t j = 0; j < 4; ++j) {
            data[i + j] = table[data[i + j]];
        }
        store_le32(&data[i], load_le32(&data[i]) ^ keys.next());
    }
}

void scramble(std::span<std::uint8_t> data, const SubstitutionTable& table, std::uint32_t seed) {
    if (data.size() % 4 != 0) {
        throw std::invalid_argument("scramble: size must be a multiple of 4");
    }
    SubstitutionTable inverse{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        inverse[table[i]] = static_cast<std::uint8_t>(i);
    }
    Keystream keys{seed};
    for (std::size_t i = 0; i < data.size(); i += 4) {
        store_le32(&data[i], load_le32(&data[i]) ^ keys.next());
        for (std::size_t j = 0; j < 4; ++j) {
            data[i + j] = inverse[data[i + j]];
        }
    }
}

std::vector<SubstitutionTable> find_substitution_table_candidates(std::span<const std::uint8_t> executable) {
    std::vector<SubstitutionTable> candidates;
    if (executable.size() < 256) {
        return candidates;
    }
    for (std::size_t offset = 0; offset + 256 <= executable.size(); offset += 4) {
        bool seen[256]{};
        bool permutation = true;
        for (std::size_t i = 0; i < 256; ++i) {
            const auto b = executable[offset + i];
            if (seen[b]) {
                permutation = false;
                break;
            }
            seen[b] = true;
        }
        if (permutation) {
            SubstitutionTable table{};
            std::memcpy(table.data(), executable.data() + offset, 256);
            candidates.push_back(table);
        }
    }
    return candidates;
}

std::optional<DataBin> DataBin::open(Reader reader, std::uint64_t total_size,
                                     const std::vector<SubstitutionTable>& candidates) {
    if (total_size == 0 || total_size % kBlockSize != 0 || total_size / kBlockSize > 0xFFFFFFFFull) {
        return std::nullopt;
    }
    const auto total_blocks = static_cast<std::uint32_t>(total_size / kBlockSize);

    for (const auto& table : candidates) {
        // The first entry is the first block of file 0, i.e. the header length in blocks.
        auto probe = reader(0, 4);
        unscramble(probe, table, 0);
        const std::uint32_t header_blocks = load_le32(probe.data());
        if (header_blocks == 0 || header_blocks >= total_blocks) {
            continue;
        }

        auto header = reader(0, static_cast<std::size_t>(header_blocks) * kBlockSize);
        unscramble(header, table, 0);
        const std::size_t word_count = header.size() / 4;
        auto word = [&](std::size_t i) { return load_le32(header.data() + i * 4); };

        // Monotonic block table terminated by the sentinel (total block count).
        std::size_t sentinel = 0;
        bool consistent = true;
        for (std::size_t i = 1; i < word_count; ++i) {
            if (word(i) < word(i - 1)) {
                consistent = false;
                break;
            }
            if (word(i) == total_blocks) {
                sentinel = i;
                break;
            }
        }
        if (!consistent || sentinel == 0) {
            continue;
        }
        const auto file_count = static_cast<std::uint32_t>(sentinel);

        // Exact sizes for files that do not fill their last block: ascending (id, size) pairs.
        std::map<std::uint32_t, std::uint32_t> exact_sizes;
        std::uint32_t previous_id = 0;
        for (std::size_t i = sentinel + 1; i + 1 < word_count; i += 2) {
            const std::uint32_t id = word(i);
            const std::uint32_t size = word(i + 1);
            if (id >= file_count || (!exact_sizes.empty() && id <= previous_id) || size == 0 ||
                size > (word(id + 1) - word(id)) * kBlockSize) {
                break;
            }
            exact_sizes[id] = size;
            previous_id = id;
        }

        std::vector<FileEntry> files;
        files.reserve(file_count);
        for (std::uint32_t id = 0; id < file_count; ++id) {
            const auto it = exact_sizes.find(id);
            const std::uint32_t block_span = (word(id + 1) - word(id)) * kBlockSize;
            files.push_back(FileEntry{id, word(id), it != exact_sizes.end() ? it->second : block_span});
        }
        return DataBin{std::move(reader), table, std::move(files)};
    }
    return std::nullopt;
}

std::vector<std::uint8_t> DataBin::read(std::uint32_t id, std::optional<std::uint32_t> size) const {
    if (id >= files_.size()) {
        throw std::out_of_range("DATA.BIN file id out of range");
    }
    const auto& entry = files_[id];
    const std::uint32_t wanted = size ? std::min(*size, entry.size) : entry.size;
    const std::uint32_t padded = (wanted + 3u) & ~3u;
    auto bytes = reader_(static_cast<std::uint64_t>(entry.first_block) * kBlockSize, padded);
    unscramble(bytes, table_, entry.first_block);
    bytes.resize(wanted);
    return bytes;
}

std::optional<OverlayHeader> parse_overlay_header(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < OverlayHeader::kSize || std::memcmp(bytes.data(), "MWo3", 4) != 0) {
        return std::nullopt;
    }
    OverlayHeader header;
    header.overlay_id = load_le32(&bytes[4]);
    header.load_address = load_le32(&bytes[8]);
    header.text_size = load_le32(&bytes[12]);
    header.data_size = load_le32(&bytes[16]);
    header.bss_size = load_le32(&bytes[20]);
    header.static_init_start = load_le32(&bytes[24]);
    header.static_init_end = load_le32(&bytes[28]);
    std::memcpy(header.name, &bytes[32], 32);
    header.name[32] = '\0';
    return header;
}

} // namespace mhf::mhfu
