// Tests DATA.BIN decoding with a fully synthetic container and a made-up substitution table (no game data).

#include "data_bin.hpp"

#include "check.hpp"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace mhf::mhfu;

namespace {

constexpr std::uint32_t kBlock = DataBin::kBlockSize;

void put_le32(std::vector<std::uint8_t>& v, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        v[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

SubstitutionTable make_table(unsigned seed) {
    SubstitutionTable table{};
    std::iota(table.begin(), table.end(), std::uint8_t{0});
    std::mt19937 rng{seed};
    std::shuffle(table.begin(), table.end(), rng);
    return table;
}

struct Synthetic {
    std::vector<std::uint8_t> container;
    std::vector<std::uint8_t> overlay;
};

// Layout: header (1 block), file 0 (1 block, exact size 100), file 1 = overlay (2 blocks), file 2 (1 block).
Synthetic build(const SubstitutionTable& table) {
    Synthetic s;
    const std::vector<std::uint32_t> first_blocks{1, 2, 4, 5};
    s.container.assign(5 * kBlock, 0);

    std::vector<std::uint8_t> header(kBlock, 0);
    for (std::size_t i = 0; i < first_blocks.size(); ++i) {
        put_le32(header, i * 4, first_blocks[i]);
    }
    put_le32(header, 16, 0);   // pair: file 0 ...
    put_le32(header, 20, 100); // ... is exactly 100 bytes
    scramble(header, table, 0);
    std::memcpy(&s.container[0], header.data(), header.size());

    s.overlay.assign(64 + 0x100 + 0x40, 0xAB);
    std::memcpy(s.overlay.data(), "MWo3", 4);
    put_le32(s.overlay, 4, 7);           // overlay id
    put_le32(s.overlay, 8, 0x09D63000);  // load address
    put_le32(s.overlay, 12, 0x100);      // text size
    put_le32(s.overlay, 16, 0x40);       // data size
    put_le32(s.overlay, 20, 0x80);       // bss size
    put_le32(s.overlay, 24, 0);
    put_le32(s.overlay, 28, 0);
    std::memset(&s.overlay[32], 0, 32);
    std::memcpy(&s.overlay[32], "stage042.ovl", 12);

    std::vector<std::uint8_t> file1(2 * kBlock, 0);
    std::memcpy(file1.data(), s.overlay.data(), s.overlay.size());
    scramble(file1, table, first_blocks[1]);
    std::memcpy(&s.container[first_blocks[1] * kBlock], file1.data(), file1.size());

    std::vector<std::uint8_t> file0(kBlock, 0x11);
    scramble(file0, table, first_blocks[0]);
    std::memcpy(&s.container[first_blocks[0] * kBlock], file0.data(), file0.size());
    return s;
}

} // namespace

int main() {
    // Keystream and scramble round trip.
    {
        const auto table = make_table(1);
        std::vector<std::uint8_t> data(64);
        std::iota(data.begin(), data.end(), std::uint8_t{0});
        const auto original = data;
        scramble(data, table, 1234);
        MHF_CHECK(data != original);
        unscramble(data, table, 1234);
        MHF_CHECK(data == original);

        Keystream zero{0}, explicit_default{0x23457F8D};
        bool same = true;
        for (int i = 0; i < 16; ++i) {
            same = same && zero.next() == explicit_default.next();
        }
        MHF_CHECK(same); // a zero seed half falls back to the default constants
    }

    // Candidate search finds permutations only.
    {
        const auto table = make_table(2);
        std::vector<std::uint8_t> exe(4096, 0x00);
        std::memcpy(&exe[1024], table.data(), 256);
        const auto candidates = find_substitution_table_candidates(exe);
        MHF_CHECK(candidates.size() == 1);
        MHF_CHECK(!candidates.empty() && candidates[0] == table);
    }

    // Full container decode, choosing the right table among decoys.
    {
        const auto table = make_table(3);
        const auto synthetic = build(table);
        auto reader = [&](std::uint64_t offset, std::size_t size) {
            return std::vector<std::uint8_t>(synthetic.container.begin() + static_cast<std::ptrdiff_t>(offset),
                                             synthetic.container.begin() + static_cast<std::ptrdiff_t>(offset + size));
        };
        const std::vector<SubstitutionTable> candidates{make_table(10), make_table(11), table};
        const auto data_bin = DataBin::open(reader, synthetic.container.size(), candidates);
        MHF_CHECK(data_bin.has_value());
        if (data_bin) {
            MHF_CHECK(data_bin->table() == table);
            MHF_CHECK(data_bin->files().size() == 3);
            MHF_CHECK(data_bin->files()[0].size == 100);
            MHF_CHECK(data_bin->files()[1].size == 2 * kBlock);
            MHF_CHECK(data_bin->files()[2].size == kBlock);

            const auto file0 = data_bin->read(0);
            MHF_CHECK(file0.size() == 100);
            MHF_CHECK(file0.size() == 100 && file0[0] == 0x11 && file0[99] == 0x11);

            const auto head = data_bin->read(1, OverlayHeader::kSize);
            const auto header = parse_overlay_header(head);
            MHF_CHECK(header.has_value());
            if (header) {
                MHF_CHECK(header->load_address == 0x09D63000);
                MHF_CHECK(std::string(header->name) == "stage042.ovl");
                MHF_CHECK(header->file_size() == synthetic.overlay.size());
                MHF_CHECK(data_bin->read(1, header->file_size()) == synthetic.overlay);
            }
            MHF_CHECK(!parse_overlay_header(data_bin->read(2, OverlayHeader::kSize)).has_value());
        }

        const std::vector<SubstitutionTable> wrong{make_table(20)};
        MHF_CHECK(!DataBin::open(reader, synthetic.container.size(), wrong).has_value());
        MHF_CHECK(!DataBin::open(reader, synthetic.container.size() + 1, candidates).has_value());
    }
    return mhf::test::finish("test_data_bin");
}
