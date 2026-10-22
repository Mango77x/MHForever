// AES-128 and AES-CMAC against the standards' test vectors, the PSP save-data scheme's round trips, and (only when the
// developer points it at a real save with environment variables, never in CI) a check against a save made by a PSP or
// PPSSPP: the hash its PARAM.SFO keeps must come out of the game's key.

#include "save_crypto/save_crypto.hpp"

#include "check.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace pokke::save_crypto;

std::vector<std::uint8_t> hex(const std::string &text) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    return out;
}

Block block(const std::string &text) {
    const auto bytes = hex(text);
    Block b{};
    std::copy_n(bytes.begin(), 16, b.begin());
    return b;
}

std::vector<std::uint8_t> read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// A PARAM.SFO with the two binary fields the scheme signs.
std::vector<std::uint8_t> make_sfo(const std::string &file_name) {
    const std::string names[2] = {"SAVEDATA_FILE_LIST", "SAVEDATA_PARAMS"};
    const std::uint32_t sizes[2] = {0xC60, 128};
    std::vector<std::uint8_t> out(20 + 2 * 16, 0);
    const auto put32 = [&](std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) out[at + i] = static_cast<std::uint8_t>(v >> (8 * i)); };
    const auto put16 = [&](std::size_t at, std::uint32_t v) { for (int i = 0; i < 2; ++i) out[at + i] = static_cast<std::uint8_t>(v >> (8 * i)); };
    std::string keys;
    std::uint32_t key_at[2], data_at[2], data = 0;
    for (int i = 0; i < 2; ++i) {
        key_at[i] = static_cast<std::uint32_t>(keys.size());
        keys += names[i];
        keys.push_back('\0');
        data_at[i] = data;
        data += sizes[i];
    }
    const std::uint32_t key_table = 20 + 2 * 16, data_table = (key_table + static_cast<std::uint32_t>(keys.size()) + 3u) & ~3u;
    out[0] = 0;
    out[1] = 'P';
    out[2] = 'S';
    out[3] = 'F';
    put32(4, 0x101);
    put32(8, key_table);
    put32(12, data_table);
    put32(16, 2);
    for (int i = 0; i < 2; ++i) {
        const std::size_t e = 20 + static_cast<std::size_t>(i) * 16;
        put16(e, key_at[i]);
        put16(e + 2, 0x0004);
        put32(e + 4, sizes[i]);
        put32(e + 8, sizes[i]);
        put32(e + 12, data_at[i]);
    }
    out.insert(out.end(), keys.begin(), keys.end());
    out.resize(data_table + data, 0);
    std::copy(file_name.begin(), file_name.end(), out.begin() + data_table);
    return out;
}

void test_aes() {
    // FIPS-197 appendix C.1.
    const Aes128 aes(block("000102030405060708090a0b0c0d0e0f"));
    const Block cipher = aes.encrypt(block("00112233445566778899aabbccddeeff"));
    MHF_CHECK(cipher == block("69c4e0d86a7b0430d8cdb78070b4c55a"));
    MHF_CHECK(aes.decrypt(cipher) == block("00112233445566778899aabbccddeeff"));
}

void test_cmac() {
    // RFC 4493 section 4.
    const Block key = block("2b7e151628aed2a6abf7158809cf4f3c");
    const auto message = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                             "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    MHF_CHECK(cmac(key, {}) == block("bb1d6929e95937287fa37d129b756746"));
    MHF_CHECK(cmac(key, std::span(message).first(16)) == block("070a16b46b4d4144f79bdd9dd04a287c"));
    MHF_CHECK(cmac(key, std::span(message).first(40)) == block("dfa66747de9ae63030ca32611497c827"));
    MHF_CHECK(cmac(key, message) == block("51f0bebf7e3b9d92fc49741779363cfe"));
}

void test_round_trips() {
    std::vector<std::uint8_t> plain(5000);
    for (std::size_t i = 0; i < plain.size(); ++i) plain[i] = static_cast<std::uint8_t>(i * 7 + 3);
    const Block key = block("00112233445566778899aabbccddeeff");
    const Block seed = block("a1a2a3a4a5a6a7a8a9aaabacadaeaf00");
    for (const int mode : {1, 3, 5}) {
        const Block k = mode == 1 ? Block{} : key;
        const auto file = encrypt(plain, k, mode, seed);
        MHF_CHECK(file.bytes.size() == plain.size() + 16);
        std::size_t same = 0; // the data really is scrambled: few bytes survive in place
        for (std::size_t i = 0; i < plain.size(); ++i) same += file.bytes[16 + i] == plain[i];
        MHF_CHECK(same < plain.size() / 64);
        const auto back = decrypt(file.bytes, k, mode);
        MHF_CHECK(back && *back == plain);
        MHF_CHECK(file.hash == file_hash(file.bytes, k, mode));
        const auto other = encrypt(plain, k, mode, block("0f0e0d0c0b0a09080706050403020100"));
        MHF_CHECK(other.bytes != file.bytes && decrypt(other.bytes, k, mode) == plain);
    }
    const auto file = encrypt(plain, key, 5, seed);
    MHF_CHECK(decrypt(file.bytes, block("ffffffffffffffffffffffffffffffff"), 5) != plain);
    MHF_CHECK(decrypt(file.bytes, key, 3) != plain);
    MHF_CHECK(!decrypt(std::vector<std::uint8_t>(8), key, 5));
}

void test_param_sfo_signing() {
    const std::string name = "MHP2NDG.BIN";
    auto sfo = make_sfo(name);
    MHF_CHECK(params_flags(sfo) == 0);
    MHF_CHECK(mode_from_params(0) == 0 && mode_from_params(0x41) == 5 && mode_from_params(0x21) == 3 && mode_from_params(0x01) == 1);
    const Block hash = block("0102030405060708090a0b0c0d0e0f10");
    MHF_CHECK(sign_param_sfo(sfo, name, hash, 5));
    MHF_CHECK(params_flags(sfo) == 0x41);
    MHF_CHECK(stored_file_hash(sfo, name) == hash);
    MHF_CHECK(stored_file_hash(sfo, "OTHER.BIN") == Block{});
    auto again = sfo;
    MHF_CHECK(sign_param_sfo(again, name, hash, 5) && again == sfo); // signing again gives the same hashes
    MHF_CHECK(!sign_param_sfo(sfo, "MISSING.BIN", hash, 5));
}

// MHF_TEST_SAVE_DIR=<folder with PARAM.SFO and the data file>, MHF_TEST_SAVE_FILE=<data file name>,
// MHF_TEST_SAVE_KEY=<the game's 32 hex digits>: the key must prove itself against the hash the save keeps.
void test_real_save() {
    const char *dir = std::getenv("MHF_TEST_SAVE_DIR");
    const char *file = std::getenv("MHF_TEST_SAVE_FILE");
    const char *key_text = std::getenv("MHF_TEST_SAVE_KEY");
    if (dir == nullptr || file == nullptr || key_text == nullptr) return;
    const auto sfo = read_file(std::string(dir) + "/PARAM.SFO");
    const auto data = read_file(std::string(dir) + "/" + file);
    MHF_CHECK(!sfo.empty() && !data.empty());
    if (sfo.empty() || data.empty()) return;
    const auto flags = params_flags(sfo);
    const int mode = flags ? mode_from_params(*flags) : 0;
    const Block key = block(key_text);
    const Block stored = stored_file_hash(sfo, file);
    const Block computed = file_hash(data, key, mode);
    std::printf("real save: mode %d, size %zu, hash %s the stored one\n", mode, data.size(), computed == stored ? "matches" : "DIFFERS FROM");
    MHF_CHECK(mode != 0 && computed == stored);
    const auto plain = decrypt(data, key, mode);
    MHF_CHECK(plain && plain->size() == data.size() - 16);
    if (plain) {
        std::printf("decrypted %zu bytes; first 32: ", plain->size());
        for (std::size_t i = 0; i < 32 && i < plain->size(); ++i) std::printf("%02x", (*plain)[i]);
        std::printf("\n");
    }
}

} // namespace

int main() {
    test_aes();
    test_cmac();
    test_round_trips();
    test_param_sfo_signing();
    test_real_save();
    return mhf::test::finish("test_save_crypto");
}
