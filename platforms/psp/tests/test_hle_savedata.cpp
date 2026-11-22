// The savedata HLE end to end on a Runtime without a program: a save goes out the way a PSP writes it (the data file
// encrypted with the game's key, the hashes in PARAM.SFO) and loads back; saves made earlier in plain form still load;
// a save that does not match the game's key is refused. The "game" here is synthetic (its key is made up).

#include "hle.hpp"

#include "save_crypto/save_crypto.hpp"

#include "check.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using mhf::psp::Runtime;
using mhf::psp::RuntimeOptions;
namespace sc = pokke::save_crypto;

constexpr std::uint32_t kParams = 0x08A00000u, kBuffer = 0x08B00000u, kIcon = 0x08C00000u;
// SceUtilitySavedataParam offsets (the same ones the HLE uses).
constexpr std::uint32_t kMode = 0x30, kGameName = 0x3C, kSaveName = 0x4C, kFileName = 0x64, kDataBuf = 0x74, kDataBufSize = 0x78,
                        kDataSize = 0x7C, kSfoTitle = 0x80, kSfoSavedataTitle = 0x100, kResult = 0x1C, kKey = 0x5DC;
enum : std::uint32_t { kAutoLoad = 0, kAutoSave = 1 };

std::vector<std::uint8_t> read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void write_file(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void put_text(Runtime &rt, std::uint32_t address, const std::string &text) {
    for (std::size_t i = 0; i <= text.size(); ++i) rt.memory().store8(address + static_cast<std::uint32_t>(i), i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0);
}

// A request like the game's: whole data file in one call, with or without a key.
void prepare(Runtime &rt, std::uint32_t mode, const std::vector<std::uint8_t> &data, const sc::Block *key) {
    auto &m = rt.memory();
    for (std::uint32_t i = 0; i < 0x600; i += 4) m.store32(kParams + i, 0);
    m.store32(kParams, 0x600); // size of the structure
    m.store32(kParams + kMode, mode);
    put_text(rt, kParams + kGameName, "ULES01213");
    put_text(rt, kParams + kSaveName, "<>");
    put_text(rt, kParams + kFileName, "MHP2NDG.BIN");
    put_text(rt, kParams + kSfoTitle, "Test Game");
    put_text(rt, kParams + kSfoSavedataTitle, "Game Data");
    m.store32(kParams + kDataBuf, kBuffer);
    m.store32(kParams + kDataBufSize, 0x20000);
    m.store32(kParams + kDataSize, static_cast<std::uint32_t>(data.size()));
    if (mode == kAutoSave) m.copy_in(kBuffer, data);
    if (key != nullptr) {
        for (std::uint32_t i = 0; i < 16; ++i) m.store8(kParams + kKey + i, (*key)[i]);
    }
}

std::vector<std::uint8_t> loaded(Runtime &rt) {
    const std::uint32_t size = rt.memory().load32(kParams + kDataSize);
    std::vector<std::uint8_t> out(size);
    if (size != 0) rt.memory().copy_out(kBuffer, out);
    return out;
}

} // namespace

int main() {
    const fs::path stick = fs::temp_directory_path() / "mhf_test_hle_savedata";
    fs::remove_all(stick);
    RuntimeOptions options;
    options.memstick_root = stick;
    Runtime rt(options);

    std::vector<std::uint8_t> payload(100000);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i * 13 + 5);
    const sc::Block key = {0x4a, 0x1f, 0xf3, 0x59, 0xae, 0xb6, 0xef, 0xf8, 0x1c, 0xa8, 0xcb, 0x23, 0xbc, 0xa5, 0x7b, 0xb3};
    const fs::path folder = stick / "PSP" / "SAVEDATA" / "ULES01213";

    // A save with the game's key is written encrypted, signed, and loads back.
    prepare(rt, kAutoSave, payload, &key);
    MHF_CHECK(mhf::psp::hle::execute_savedata(rt, kParams) == 0);
    const auto file = read_file(folder / "MHP2NDG.BIN");
    const auto sfo = read_file(folder / "PARAM.SFO");
    MHF_CHECK(file.size() == payload.size() + 16);
    MHF_CHECK(!std::equal(payload.begin(), payload.end(), file.begin() + 16)); // not stored as it is
    MHF_CHECK(sc::params_flags(sfo) == 0x41);                                   // mode 5, as PPSSPP writes it
    MHF_CHECK(sc::stored_file_hash(sfo, "MHP2NDG.BIN") == sc::file_hash(file, key, 5));
    prepare(rt, kAutoLoad, {}, &key);
    MHF_CHECK(mhf::psp::hle::execute_savedata(rt, kParams) == 0);
    MHF_CHECK(loaded(rt) == payload);

    // Saving again keeps the earlier generation aside (outside the folders a game lists).
    auto second = payload;
    second[0] ^= 0xFF;
    prepare(rt, kAutoSave, second, &key);
    MHF_CHECK(mhf::psp::hle::execute_savedata(rt, kParams) == 0);
    MHF_CHECK(fs::is_regular_file(stick / "PSP" / "SAVEDATA" / ".previous" / "ULES01213" / "MHP2NDG.BIN"));
    prepare(rt, kAutoLoad, {}, &key);
    MHF_CHECK(mhf::psp::hle::execute_savedata(rt, kParams) == 0 && loaded(rt) == second);

    // Another key (another game's or region's save, or a damaged file) is refused, not read as garbage.
    sc::Block other = key;
    other[0] ^= 1;
    prepare(rt, kAutoLoad, {}, &other);
    MHF_CHECK(mhf::psp::hle::execute_savedata(rt, kParams) == 0x80110306u);
    auto damaged = read_file(folder / "MHP2NDG.BIN");
    damaged[100] ^= 1;
    write_file(folder / "MHP2NDG.BIN", damaged);
    prepare(rt, kAutoLoad, {}, &key);
    MHF_CHECK(mhf::psp::hle::execute_savedata(rt, kParams) == 0x80110306u);

    // Without a key the save is plain (nothing to encrypt with), and a plain file replacing an encrypted one clears the
    // PARAM.SFO's encryption fields so the pair stays consistent; both read back.
    prepare(rt, kAutoSave, payload, nullptr);
    MHF_CHECK(mhf::psp::hle::execute_savedata(rt, kParams) == 0);
    MHF_CHECK(read_file(folder / "MHP2NDG.BIN") == payload);
    MHF_CHECK(!sc::params_flags(read_file(folder / "PARAM.SFO")).value_or(0));
    prepare(rt, kAutoLoad, {}, &key);
    MHF_CHECK(mhf::psp::hle::execute_savedata(rt, kParams) == 0 && loaded(rt) == payload);

    // And an encrypted save over a plain one works the same way round.
    prepare(rt, kAutoSave, second, &key);
    MHF_CHECK(mhf::psp::hle::execute_savedata(rt, kParams) == 0);
    MHF_CHECK(sc::params_flags(read_file(folder / "PARAM.SFO")) == 0x41);
    prepare(rt, kAutoLoad, {}, &key);
    MHF_CHECK(mhf::psp::hle::execute_savedata(rt, kParams) == 0 && loaded(rt) == second);

    // GETSIZE (mode 22) answers how much space is still missing beyond the free space: nothing for files that fit, the
    // difference for files that do not. Freedom Unite's data install (183 files, about 580 MB) stops at "not enough free
    // space" on anything but 0.
    {
        constexpr std::uint32_t kSizeInfo = 0x5FC, kInfo = 0x08A01000u, kEntries = 0x08A02000u;
        const auto get_size = [&](std::uint64_t file_bytes, std::uint32_t files) {
            prepare(rt, 22, {}, nullptr);
            put_text(rt, kParams + kGameName, "ULES01213DAT");
            put_text(rt, kParams + kSaveName, "");
            auto &m = rt.memory();
            for (std::uint32_t i = 0; i < 64; i += 4) m.store32(kInfo + i, 0);
            m.store32(kInfo + 4, files); // normal entries
            m.store32(kInfo + 12, kEntries);
            for (std::uint32_t i = 0; i < files; ++i) {
                m.store32(kEntries + i * 24, static_cast<std::uint32_t>(file_bytes));
                m.store32(kEntries + i * 24 + 4, static_cast<std::uint32_t>(file_bytes >> 32));
            }
            m.store32(kParams + kSizeInfo, kInfo);
            return mhf::psp::hle::execute_savedata(rt, kParams);
        };
        MHF_CHECK(get_size(3u << 20, 183) == 0);
        const std::uint32_t free_kb = rt.memory().load32(kInfo + 24);
        MHF_CHECK(free_kb >= 600u * 1024u);
        MHF_CHECK(rt.memory().load32(kInfo + 36) == 0 && rt.memory().load32(kInfo + 48) == 0); // fits: nothing missing
        MHF_CHECK(get_size(std::uint64_t{free_kb} * 1024u + (64u << 20), 1) == 0);
        const std::uint32_t missing = rt.memory().load32(kInfo + 36);
        MHF_CHECK(missing >= 64u * 1024u && missing < 65u * 1024u); // the 64 MB that do not fit, and the folder's few KB
        MHF_CHECK(rt.memory().load32(kInfo + 48) == missing);      // nothing there to overwrite
    }

    fs::remove_all(stick);
    return mhf::test::finish("test_hle_savedata");
}
