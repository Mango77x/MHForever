#include "iso9660.hpp"

#include "check.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kSector = mhf::psp::IsoImage::kSectorSize;

void put_le32_both(std::uint8_t* p, std::uint32_t v) { // ISO9660 "both-endian" 32-bit field
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
        p[7 - i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

std::size_t put_record(std::uint8_t* p, std::uint32_t lba, std::uint32_t size, bool dir, const std::string& name) {
    const std::size_t length = (33 + name.size() + 1) & ~std::size_t{1};
    p[0] = static_cast<std::uint8_t>(length);
    put_le32_both(p + 2, lba);
    put_le32_both(p + 10, size);
    p[25] = dir ? 0x02 : 0x00;
    p[32] = static_cast<std::uint8_t>(name.size());
    std::memcpy(p + 33, name.data(), name.size());
    return length;
}

// Builds a tiny image: /PSP_GAME/USRDIR/DATA.BIN containing "hello world!".
std::vector<std::uint8_t> build_image() {
    std::vector<std::uint8_t> image(24 * kSector, 0);
    std::uint8_t* pvd = &image[16 * kSector];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    put_record(pvd + 156, 20, kSector, true, std::string(1, '\0'));

    auto dir = [&](std::uint32_t lba, std::uint32_t parent, const std::string& child, std::uint32_t child_lba,
                   std::uint32_t child_size, bool child_dir) {
        std::uint8_t* p = &image[lba * kSector];
        p += put_record(p, lba, kSector, true, std::string(1, '\0'));
        p += put_record(p, parent, kSector, true, std::string(1, '\1'));
        put_record(p, child_lba, child_size, child_dir, child);
    };
    dir(20, 20, "PSP_GAME", 21, kSector, true);
    dir(21, 20, "USRDIR", 22, kSector, true);
    dir(22, 21, "DATA.BIN;1", 23, 12, false);
    std::memcpy(&image[23 * kSector], "hello world!", 12);
    return image;
}

} // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() / "mhf_test_iso9660.iso";
    {
        const auto image = build_image();
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(image.data()),
                                                     static_cast<std::streamsize>(image.size()));
    }
    {
        mhf::psp::IsoImage iso{path};
        MHF_CHECK(iso.entries().size() == 3);
        const auto entry = iso.find("/psp_game/usrdir/data.bin");
        MHF_CHECK(entry.has_value());
        if (entry) {
            MHF_CHECK(entry->lba == 23);
            MHF_CHECK(entry->size == 12);
            const auto bytes = iso.read_file(*entry, 6, 6);
            MHF_CHECK(std::string(bytes.begin(), bytes.end()) == "world!");
            bool threw = false;
            try {
                (void)iso.read_file(*entry, 8, 8);
            } catch (const std::exception&) {
                threw = true;
            }
            MHF_CHECK(threw);
        }
        MHF_CHECK(!iso.find("/PSP_GAME/SYSDIR/EBOOT.BIN").has_value());
    }
    std::filesystem::remove(path);

    bool rejected = false;
    const auto bogus = std::filesystem::temp_directory_path() / "mhf_test_not_iso.bin";
    std::ofstream(bogus, std::ios::binary).write(std::string(20 * kSector, 'x').data(), 20 * kSector);
    try {
        mhf::psp::IsoImage iso{bogus};
    } catch (const std::exception&) {
        rejected = true;
    }
    std::filesystem::remove(bogus);
    MHF_CHECK(rejected);
    return mhf::test::finish("test_iso9660");
}
