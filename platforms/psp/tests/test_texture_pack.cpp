// Texture packs (frontend/texture_pack.*, texture_images.*): the quick hash against an independent statement of it,
// the bytes a key covers, the palette in the key, the ini's sections and wildcard keys, choosing the ini for a release,
// finding a pack inside a folder, reviewing it, and decoding images on the loader's threads. Everything is made in a
// temporary folder from synthetic data.

#include "texture_images.hpp"
#include "texture_pack.hpp"

#include "check.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace {

using namespace mhf::psp::textures;
namespace fs = std::filesystem;

// The quick hash as Yakumo states it (four 32-bit lanes with 16-bit products, per-lane seeds), written independently
// of quick_hash(): both must agree on every input.
std::uint32_t quick_reference(const std::uint8_t *data, std::size_t size, bool aligned) {
    const auto word = [&](std::size_t o) {
        return std::uint32_t(data[o]) | std::uint32_t(data[o + 1]) << 8 | std::uint32_t(data[o + 2]) << 16 | std::uint32_t(data[o + 3]) << 24;
    };
    std::uint32_t result = 0;
    if (!aligned || size % 64u != 0u) {
        for (std::size_t pair = 0; pair < size / 8u; ++pair) result = (result + word(pair * 8u)) ^ word(pair * 8u + 4u);
        return result;
    }
    constexpr std::uint32_t seeds[] = {0x9bd9c00bu, 0xb6514b73u, 0x43094d9bu, 0x00010083u};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        std::uint32_t value = 0;
        const auto weights = [&](std::size_t block) {
            const auto increment = static_cast<std::uint32_t>(block) * 0x2455u;
            return ((seeds[lane] + increment) & 0xffffu) | (((seeds[lane] >> 16u) + increment) << 16u);
        };
        const auto multiply = [](std::uint32_t a, std::uint32_t b) { return ((a * b) & 0xffffu) | (((a >> 16u) * (b >> 16u)) << 16u); };
        for (std::size_t block = 0; block < size / 64u; ++block) {
            const std::size_t offset = block * 64u + lane * 4u;
            const std::uint32_t product = multiply(word(offset), weights(block));
            value = ((value + product) & 0xffffu) | (((value >> 16u) + (product >> 16u)) << 16u);
            value = ((value ^ word(offset + 16u)) + word(offset + 32u)) ^ multiply(word(offset + 48u), weights(block));
        }
        result += value + weights(size / 64u);
    }
    return result;
}

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("mhf_texture_pack_test_" + std::to_string(std::random_device{}()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write(const fs::path &file, const std::string &text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

// A small PNG: a 4x4 gradient.
void write_test_png(const fs::path &file, std::uint32_t side) {
    fs::create_directories(file.parent_path());
    std::vector<std::uint32_t> px(side * side);
    for (std::uint32_t i = 0; i < px.size(); ++i) px[i] = 0xFF000000u | (i * 16u);
    MHF_CHECK(write_png(file, side, side, px.data()));
}

} // namespace

int main() {
    std::mt19937 rng(1234);
    alignas(16) static std::uint8_t memory[1 << 16];
    for (auto &b : memory) b = static_cast<std::uint8_t>(rng());
    const MemoryReader reader = [](std::uint32_t address, std::uint32_t size) -> const std::uint8_t * {
        return address + std::uint64_t(size) <= sizeof memory ? memory + address : nullptr;
    };

    // The quick hash: aligned whole blocks, other sizes, unaligned data.
    for (const std::uint32_t size : {0u, 8u, 64u, 128u, 4096u, 8192u, 72u, 100u, 63u}) {
        MHF_CHECK(quick_hash(memory, size, true) == quick_reference(memory, size, true));
        MHF_CHECK(quick_hash(memory + 4, size, false) == quick_reference(memory + 4, size, false));
    }

    TempDir dir;

    // A pack: options, wildcard and full keys, mip levels, an ignored texture, an image outside the pack, ranges,
    // filtering, games, and a BOM.
    write(dir.path / "pack" / "textures.ini",
          "\xEF\xBB\xBF[options]\nversion = 1\nhash = quick\n\n[hashes]\n"
          "000000000000000011111111 = a.png\n"
          "0000000000000000aaaaaaaa = Sub\\B.png\n"
          "0000000000000000aaaaaaaa_1 = sub/b_1.png\n"
          "0000000000000000aaaaaaaa_3 = sub/b_3.png\n"
          "0880000000000000cccccccc = full.png\n"
          "000000000000000022222222 = \n"
          "000000000000000033333333 = ../outside.png\n"
          "not a key = x.png\n"
          "[hashranges]\n08800000,64,64 = 48,32\n"
          "[filtering]\n000000000000000011111111 = nearest\n"
          "[games]\nULES01213 = true\n");
    write_test_png(dir.path / "pack" / "a.png", 4);
    write_test_png(dir.path / "pack" / "sub" / "b.png", 8);
    write_test_png(dir.path / "pack" / "sub" / "b_1.png", 4);
    std::string error;
    auto pack = Pack::open(dir.path / "pack", "ULES01213", error);
    MHF_CHECK(pack != nullptr);
    if (pack) {
        MHF_CHECK(pack->options().hash == HashKind::Quick);
        Match m = pack->find(Key{0x0880000000000909ull, 0x11111111u}); // any address: the data hash alone
        MHF_CHECK(m.entry != nullptr && m.entry->image() == "a.png" && m.filter == Filter::Nearest);
        m = pack->find(Key{0x1234, 0xaaaaaaaau});
        MHF_CHECK(m.entry != nullptr && m.entry->levels.size() == 2); // level 3 does not follow level 1
        MHF_CHECK(m.entry->image() == "Sub/B.png");
        MHF_CHECK(fs::exists(pack->path_of(m.entry->image()))); // found again in another case where that matters
        m = pack->find(Key{0x0880000000000000ull, 0xccccccccu});
        MHF_CHECK(m.entry != nullptr && m.entry->image() == "full.png");
        MHF_CHECK(pack->find(Key{0x0990000000000000ull, 0xccccccccu}).entry == nullptr); // another address
        m = pack->find(Key{0, 0x22222222u});
        MHF_CHECK(m.entry != nullptr && m.entry->keeps_original());
        MHF_CHECK(pack->find(Key{0, 0x33333333u}).entry == nullptr); // outside the pack: refused
        MHF_CHECK(pack->find(Key{0, 0x44444444u}).entry == nullptr);

        // The bytes a key covers: a 64x64 16-bit texture, contiguous.
        TextureFacts t;
        t.address = 0x1000;
        t.buffer_width = 64;
        t.format = 0;
        t.width = t.height = 64;
        t.dimension = 6 | 6 << 8;
        Key key;
        std::uint32_t cw = 0, ch = 0;
        MHF_CHECK(pack->key_of(t, reader, key, cw, ch));
        MHF_CHECK(key.data == quick_hash(memory + 0x1000, 64 * 64 * 2, true));
        MHF_CHECK(key.cache == (std::uint64_t(0x1000) << 32 | t.dimension) && cw == 64 && ch == 64);

        // Rows narrower than the buffer: each row on its own, folded.
        t.buffer_width = 128;
        MHF_CHECK(pack->key_of(t, reader, key, cw, ch));
        std::uint32_t folded = 0;
        for (std::uint32_t y = 0; y < 64; ++y) folded = folded * 11u ^ quick_hash(memory + 0x1000 + y * 256, 128, true);
        MHF_CHECK(key.data == folded);

        // A buffer width below one 16-byte unit counts as one unit.
        t.buffer_width = 3;
        MHF_CHECK(pack->key_of(t, reader, key, cw, ch));
        MHF_CHECK(key.data == quick_hash(memory + 0x1000, (8 * 64 + (64 - 8)) * 2, true));

        // A 512-tall texture drawn in through mode down to row 100 is hashed down to it.
        t.buffer_width = 16;
        t.width = 16;
        t.height = 512;
        t.max_seen_v = 100;
        MHF_CHECK(pack->key_of(t, reader, key, cw, ch));
        MHF_CHECK(key.data == quick_hash(memory + 0x1000, 16 * 100 * 2, true) && ch == 512);
        t.max_seen_v = 0;

        // The palette in the key: XXH32 of what the last load and the index offset reach, with the CLUT format word.
        std::uint8_t clut[1024];
        for (auto &b : clut) b = static_cast<std::uint8_t>(rng());
        t.format = 5; // CLUT8
        t.width = t.height = 16;
        t.buffer_width = 16;
        t.clut = clut;
        t.clut_format_word = 0xC5000000u | 1u << 16 | 0xFFu << 8 | 2u; // 16-bit 4444, index offset 16 entries
        t.clut_last_bytes = 256;
        t.clut_max_bytes = 1024;
        MHF_CHECK(pack->key_of(t, reader, key, cw, ch));
        const std::uint32_t clut_hash = XXH32(clut, 256 + 16 * 2, 0xC0108888u) ^ t.clut_format_word;
        MHF_CHECK(key.cache == ((std::uint64_t(0x1000) << 32 | t.dimension) ^ clut_hash));

        // A [hashranges] entry: only that part is hashed, and the image covers only it.
        t.format = 3;
        t.address = 0x08800000;
        t.width = t.height = 64;
        t.buffer_width = 64;
        const MemoryReader high = [](std::uint32_t address, std::uint32_t size) -> const std::uint8_t * {
            return address >= 0x08800000u && address - 0x08800000u + std::uint64_t(size) <= sizeof memory ? memory + (address - 0x08800000u) : nullptr;
        };
        MHF_CHECK(pack->key_of(t, high, key, cw, ch));
        MHF_CHECK(cw == 48 && ch == 32);
        std::uint32_t rows = 0; // 48 of the buffer's 64 texels a row: hashed row by row
        for (std::uint32_t y = 0; y < 32; ++y) rows = rows * 11u ^ quick_hash(memory + y * 256, 192, true);
        MHF_CHECK(key.data == rows);
    }

    // Review: keys, distinct images, the missing ones.
    {
        Review review;
        MHF_CHECK(Pack::review(dir.path / "pack", "ULES01213", review, error));
        MHF_CHECK(review.keys == 4);
        MHF_CHECK(review.images == 4 && review.missing == 1); // full.png is not there
    }

    // One ini per release, as KHD has them: the one named for the release's region, else textures.ini.
    write(dir.path / "khd" / "MAIN" / "textures.ini", "[options]\nhash = quick\n[hashes]\n000000000000000000000001 = jp.png\n");
    write(dir.path / "khd" / "MAIN" / "textures_FU_US.ini", "[options]\nhash = quick\n[hashes]\n000000000000000000000002 = us.png\n");
    write(dir.path / "khd" / "MAIN" / "textures_FU_EU.ini", "[options]\nhash = quick\n[hashes]\n000000000000000000000003 = eu.png\n");
    write(dir.path / "khd" / "ADDONS" / "readme.txt", "-");
    const fs::path main = Pack::locate(dir.path / "khd", "ULES01213");
    MHF_CHECK(main == dir.path / "khd" / "MAIN");
    for (const auto &[id, wanted] : std::vector<std::pair<std::string, std::uint32_t>>{{"ULES01213", 3}, {"ULUS10391", 2}, {"ULJM05500", 1}}) {
        auto p = Pack::open(main, id, error);
        MHF_CHECK(p != nullptr && p->find(Key{0, wanted}).entry != nullptr && p->size() == 1);
    }
    // PPSSPP's memory stick layout.
    write(dir.path / "stick" / "PSP" / "TEXTURES" / "ULUS10391" / "textures.ini", "[options]\nhash = xxh64\n[hashes]\n");
    MHF_CHECK(Pack::locate(dir.path / "stick", "ULUS10391") == dir.path / "stick" / "PSP" / "TEXTURES" / "ULUS10391");
    // An unsupported hash is refused with the reason.
    write(dir.path / "bad" / "textures.ini", "[options]\nhash = crc\n[hashes]\n000000000000000000000001 = a.png\n");
    MHF_CHECK(Pack::open(dir.path / "bad", "ULES01213", error) == nullptr && error.find("crc") != std::string::npos);
    // ignoreAddress with the quick hash is turned off, as PPSSPP does.
    write(dir.path / "ign" / "textures.ini", "[options]\nhash = quick\nignoreAddress = true\n[hashes]\n000000000000000000000001 = a.png\n");
    auto ign = Pack::open(dir.path / "ign", "", error);
    MHF_CHECK(ign && !ign->options().ignore_address);

    // Images: a PNG round trip, mip levels given as files, and the loader's threads.
    {
        Image image;
        MHF_CHECK(decode_levels({dir.path / "pack" / "sub" / "b.png", dir.path / "pack" / "sub" / "b_1.png"}, image, error));
        MHF_CHECK(image.width == 8 && image.height == 8 && image.levels.size() == 2 && image.levels[1].size() == 4 * 4 * 4);
        std::uint32_t first = 0;
        std::memcpy(&first, image.levels[0].data() + 4, 4);
        MHF_CHECK(first == (0xFF000000u | 16u));
        MHF_CHECK(!decode_image(dir.path / "pack" / "textures.ini", image, error));

        Loader loader(2);
        std::shared_ptr<const Image> got;
        MHF_CHECK(loader.get("a", {dir.path / "pack" / "a.png"}, got) == Loader::State::Loading);
        MHF_CHECK(loader.get("missing", {dir.path / "pack" / "full.png"}, got) == Loader::State::Loading);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        Loader::State a = Loader::State::Loading, b = Loader::State::Loading;
        while ((a == Loader::State::Loading || b == Loader::State::Loading) && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            a = loader.get("a", {dir.path / "pack" / "a.png"}, got);
            std::shared_ptr<const Image> none;
            b = loader.get("missing", {dir.path / "pack" / "full.png"}, none);
        }
        MHF_CHECK(a == Loader::State::Ready && got && got->width == 4);
        MHF_CHECK(b == Loader::State::Failed);
        loader.forget("a");
        MHF_CHECK(loader.get("a", {dir.path / "pack" / "a.png"}, got) == Loader::State::Loading); // decoded again on demand
    }

    return mhf::test::finish("texture_pack");
}
