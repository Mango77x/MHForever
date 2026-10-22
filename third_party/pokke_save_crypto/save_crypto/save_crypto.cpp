#include "save_crypto/save_crypto.hpp"

#include <algorithm>
#include <cstring>

namespace pokke::save_crypto {

namespace {

// KIRK's fixed AES keys, by the key slot the save-data scheme selects.
// (Constants: the PSP's key slots 3, 4, 5, 12, 13, 14, 16, 17, 18, 0x53,
// 0x57 and 0x64.)
struct Slot {
    int number;
    Block key;
};
constexpr Slot kSlots[] = {
    {3, {0x98, 0x02, 0xC4, 0xE6, 0xEC, 0x9E, 0x9E, 0x2F, 0xFC, 0x63, 0x4C, 0xE4, 0x2F, 0xBB, 0x46, 0x68}},
    {4, {0x99, 0x24, 0x4C, 0xD2, 0x58, 0xF5, 0x1B, 0xCB, 0xB0, 0x61, 0x9C, 0xA7, 0x38, 0x30, 0x07, 0x5F}},
    {5, {0x02, 0x25, 0xD7, 0xBA, 0x63, 0xEC, 0xB9, 0x4A, 0x9D, 0x23, 0x76, 0x01, 0xB3, 0xF6, 0xAC, 0x17}},
    {12, {0x84, 0x85, 0xC8, 0x48, 0x75, 0x08, 0x43, 0xBC, 0x9B, 0x9A, 0xEC, 0xA7, 0x9C, 0x7F, 0x60, 0x18}},
    {13, {0xB5, 0xB1, 0x6E, 0xDE, 0x23, 0xA9, 0x7B, 0x0E, 0xA1, 0x7C, 0xDB, 0xA2, 0xDC, 0xDE, 0xC4, 0x6E}},
    {14, {0xC8, 0x71, 0xFD, 0xB3, 0xBC, 0xC5, 0xD2, 0xF2, 0xE2, 0xD7, 0x72, 0x9D, 0xDF, 0x82, 0x68, 0x82}},
    {16, {0x32, 0x29, 0x5B, 0xD5, 0xEA, 0xF7, 0xA3, 0x42, 0x16, 0xC8, 0x8E, 0x48, 0xFF, 0x50, 0xD3, 0x71}},
    {17, {0x46, 0xF2, 0x5E, 0x8E, 0x4D, 0x2A, 0xA5, 0x40, 0x73, 0x0B, 0xC4, 0x6E, 0x47, 0xEE, 0x6F, 0x0A}},
    {18, {0x5D, 0xC7, 0x11, 0x39, 0xD0, 0x19, 0x38, 0xBC, 0x02, 0x7F, 0xDD, 0xDC, 0xB0, 0x83, 0x7D, 0x9D}},
    {0x53, {0xAF, 0xFE, 0x8E, 0xB1, 0x3D, 0xD1, 0x7E, 0xD8, 0x0A, 0x61, 0x24, 0x1C, 0x95, 0x92, 0x56, 0xB6}},
    {0x57, {0x1C, 0x9B, 0xC4, 0x90, 0xE3, 0x06, 0x64, 0x81, 0xFA, 0x59, 0xFD, 0xB6, 0x00, 0xBB, 0x28, 0x70}},
    {0x64, {0x03, 0xB3, 0x02, 0xE8, 0x5F, 0xF3, 0x81, 0xB1, 0x3B, 0x8D, 0xAA, 0x2A, 0x90, 0xFF, 0x5E, 0x61}},
};

// The save-data scheme's own masks (constants of the firmware).
constexpr Block kMacMask3 = {0xFA, 0xAA, 0x50, 0xEC, 0x2F, 0xDE, 0x54, 0x93, 0xAD, 0x14, 0xB2, 0xCE, 0xA5, 0x30, 0x05, 0xDF};
constexpr Block kMacMask5 = {0xCB, 0x15, 0xF4, 0x07, 0xF9, 0x6A, 0x52, 0x3C, 0x04, 0xB9, 0xB2, 0xEE, 0x5C, 0x53, 0xFA, 0x86};
constexpr Block kSeedMask3 = {0x36, 0xA5, 0x3E, 0xAC, 0xC5, 0x26, 0x9E, 0xA3, 0x83, 0xD9, 0xEC, 0x25, 0x6C, 0x48, 0x48, 0x72};
constexpr Block kHeaderMask3 = {0xD8, 0xC0, 0xB0, 0xF3, 0x3E, 0x6B, 0x76, 0x85, 0xFD, 0xFB, 0x4D, 0x7D, 0x45, 0x1E, 0x92, 0x03};
constexpr Block kSeedMask5 = {0x70, 0x44, 0xA3, 0xAE, 0xEF, 0x5D, 0xA5, 0xF2, 0x85, 0x7F, 0xF2, 0xD6, 0x94, 0xF5, 0x36, 0x3B};
constexpr Block kHeaderMask5 = {0xEC, 0x6D, 0x29, 0x59, 0x26, 0x35, 0xA5, 0x7F, 0x97, 0x2A, 0x0D, 0xBC, 0xA3, 0x26, 0x33, 0x00};

const Block &slot_key(int number) {
    for (const Slot &s : kSlots)
        if (s.number == number) return s.key;
    return kSlots[0].key; // unreachable: every caller passes a listed slot
}

// What each mode uses. Modes 2, 4 and 6 are the console-bound twins of 1,
// 3 and 5: same keys for the data, but their MACs need the fuse keys.
struct ModeKeys {
    int mac_slot;    // CMAC key for the file and PARAM.SFO hashes
    int header_slot; // turns the random seed into the header and back
    int stream_slot; // makes the keystream from counter blocks
    const Block *mac_mask;
    const Block *seed_mask, *header_mask;
};

ModeKeys keys_for(int mode) {
    switch (mode) {
    case 1: return {3, 4, 0x53, nullptr, nullptr, nullptr};
    case 2: return {5, 4, 0x53, nullptr, nullptr, nullptr};
    case 3: return {12, 14, 0x57, &kMacMask3, &kSeedMask3, &kHeaderMask3};
    case 4: return {13, 14, 0x57, &kMacMask3, &kSeedMask3, &kHeaderMask3};
    case 6: return {17, 18, 0x64, &kMacMask5, &kSeedMask5, &kHeaderMask5};
    default: return {16, 18, 0x64, &kMacMask5, &kSeedMask5, &kHeaderMask5}; // 5
    }
}

bool console_bound(int mode) { return mode == 2 || mode == 4 || mode == 6; }

Block masked(const Block &b, const Block *mask) { return mask ? xor_blocks(b, *mask) : b; }

bool is_zero(const Block &b) {
    return std::all_of(b.begin(), b.end(), [](std::uint8_t v) { return v == 0; });
}

void put_le32(std::uint8_t *p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// XORs `data` (a multiple of 16 bytes) with the keystream of a file whose
// header is `header`. Keystream block n (counting from 1) is the stream key's
// AES decryption of the counter block base[0..11] | n, XORed with the counter
// block before it (all zero before block 1). The base comes from the header,
// unmasked with the game's key.
void apply_keystream(std::vector<std::uint8_t> &data, const Block &header, const Block &key, int mode) {
    const ModeKeys k = keys_for(mode);
    const Block seed = masked(Aes128(slot_key(k.header_slot)).decrypt(masked(xor_blocks(header, key), k.header_mask)),
                              k.seed_mask);
    const Aes128 stream(slot_key(k.stream_slot));
    Block previous{};
    for (std::size_t at = 0, n = 1; at < data.size(); at += 16, ++n) {
        Block counter = seed;
        put_le32(counter.data() + 12, static_cast<std::uint32_t>(n));
        const Block pad = xor_blocks(stream.decrypt(counter), previous);
        for (std::size_t i = 0; i < 16; ++i) data[at + i] ^= pad[i];
        previous = counter;
    }
}

// The firmware's MAC: AES-CMAC under the mode's key, masked by mode, then
// (when the caller has a key) mixed with that key. Nothing for the
// console-bound modes.
std::optional<Block> mac(std::span<const std::uint8_t> message, int mode, const Block *key) {
    const ModeKeys k = keys_for(mode);
    const Block &mac_key = slot_key(k.mac_slot);
    Block t = masked(cmac(mac_key, message), k.mac_mask);
    if (console_bound(mode)) return std::nullopt;
    if (key != nullptr) t = Aes128(mac_key).encrypt(xor_blocks(t, *key));
    return t;
}

std::vector<std::uint8_t> padded(std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> out(bytes.begin(), bytes.end());
    out.resize((out.size() + 15) & ~std::size_t{15}, 0);
    return out;
}

// PARAM.SFO: where a key's value is, and its length.
struct Field {
    std::size_t offset = 0, length = 0;
};

std::optional<Field> find_field(std::span<const std::uint8_t> sfo, const std::string &name) {
    const auto le32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(sfo[at] | (sfo[at + 1] << 8) | (sfo[at + 2] << 16) |
                                          (static_cast<std::uint32_t>(sfo[at + 3]) << 24));
    };
    if (sfo.size() < 20 || std::memcmp(sfo.data(), "\0PSF", 4) != 0) return std::nullopt;
    const std::uint32_t keys = le32(8), values = le32(12), count = le32(16);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t entry = 20 + std::size_t{i} * 16;
        if (entry + 16 > sfo.size()) return std::nullopt;
        const std::size_t key_at = keys + (sfo[entry] | (sfo[entry + 1] << 8));
        const std::uint32_t max_length = le32(entry + 8), offset = le32(entry + 12);
        if (key_at >= sfo.size()) return std::nullopt;
        const char *text = reinterpret_cast<const char *>(sfo.data() + key_at);
        const std::size_t text_length = strnlen(text, sfo.size() - key_at);
        if (std::string(text, text_length) != name) continue;
        const std::size_t at = values + offset;
        if (at + max_length > sfo.size()) return std::nullopt;
        return Field{at, max_length};
    }
    return std::nullopt;
}

} // namespace

int mode_from_params(std::uint8_t first) {
    if (first == 0) return 0;
    const int bits = first >> 4;
    return (bits & 4) ? 5 : (bits & 2) ? 3 : 1;
}

EncryptedFile encrypt(std::span<const std::uint8_t> plain, const Block &key, int mode, const Block &iv_seed) {
    const ModeKeys k = keys_for(mode);
    // The header: 12 random bytes and a zero counter, masked, encrypted, masked.
    Block seed = iv_seed;
    std::fill(seed.begin() + 12, seed.end(), 0);
    const Block header =
        masked(Aes128(slot_key(k.header_slot)).encrypt(masked(seed, k.seed_mask)), k.header_mask);

    std::vector<std::uint8_t> data = padded(plain);
    apply_keystream(data, header, is_zero(key) ? Block{} : key, mode);
    EncryptedFile out;
    out.bytes.assign(header.begin(), header.end());
    out.bytes.insert(out.bytes.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(plain.size()));
    out.hash = file_hash(out.bytes, key, mode);
    return out;
}

std::optional<std::vector<std::uint8_t>> decrypt(std::span<const std::uint8_t> file, const Block &key, int mode) {
    if (file.size() < 16) return std::nullopt;
    Block header{};
    std::copy_n(file.begin(), 16, header.begin());
    std::vector<std::uint8_t> data = padded(file.subspan(16));
    apply_keystream(data, header, is_zero(key) ? Block{} : key, mode);
    data.resize(file.size() - 16);
    return data;
}

Block file_hash(std::span<const std::uint8_t> file, const Block &key, int mode) {
    // Over the header and the encrypted data, zero-padded to 16 bytes.
    const std::vector<std::uint8_t> message = padded(file);
    return mac(message, mode, is_zero(key) ? nullptr : &key).value_or(Block{});
}

Block stored_file_hash(std::span<const std::uint8_t> sfo, const std::string &file_name) {
    Block hash{};
    const auto list = find_field(sfo, "SAVEDATA_FILE_LIST");
    if (!list) return hash;
    for (std::size_t entry = list->offset; entry + 0x20 <= list->offset + list->length; entry += 0x20) {
        const char *name = reinterpret_cast<const char *>(sfo.data() + entry);
        if (std::string(name, strnlen(name, 13)) != file_name) continue;
        std::copy_n(sfo.begin() + static_cast<std::ptrdiff_t>(entry + 0x0D), 16, hash.begin());
        break;
    }
    return hash;
}

std::optional<std::uint8_t> params_flags(std::span<const std::uint8_t> sfo) {
    const auto params = find_field(sfo, "SAVEDATA_PARAMS");
    if (!params || params->length == 0) return std::nullopt;
    return sfo[params->offset];
}

bool sign_param_sfo(std::vector<std::uint8_t> &sfo, const std::string &file_name, const Block &hash, int mode) {
    const auto params = find_field(sfo, "SAVEDATA_PARAMS");
    const auto list = find_field(sfo, "SAVEDATA_FILE_LIST");
    if (!params || !list || params->length < 0x80) return false;

    // The file's entry: 13 bytes of name, then its 16-byte hash.
    bool found = false;
    for (std::size_t entry = list->offset; entry + 0x20 <= list->offset + list->length; entry += 0x20) {
        const char *name = reinterpret_cast<const char *>(sfo.data() + entry);
        if (std::string(name, strnlen(name, 13)) != file_name) continue;
        std::copy(hash.begin(), hash.end(), sfo.begin() + static_cast<std::ptrdiff_t>(entry + 0x0D));
        found = true;
        break;
    }
    if (!found) return false;

    // Each hash covers the whole PARAM.SFO as it is at that moment, in this
    // order: the console-bound one (filled with 0x01 bytes: it needs a PSP's
    // fuse keys), then the mode's own, then the mode 1 one.
    std::uint8_t *p = sfo.data() + params->offset;
    std::fill(p, p + 0x80, 0);
    const auto put = [&](std::size_t at, int hash_mode) {
        const Block h = mac(sfo, hash_mode, nullptr).value_or([] {
            Block ones{};
            ones.fill(0x01);
            return ones;
        }());
        std::copy(h.begin(), h.end(), p + at);
    };
    if (mode == 5) {
        put(0x20, 6);
        p[0] |= 0x41;
        put(0x70, 5);
    } else if (mode == 3) {
        put(0x20, 4);
        p[0] |= 0x21;
        put(0x70, 3);
    } else {
        put(0x20, 2);
        p[0] |= 0x01;
    }
    put(0x10, 1);
    return true;
}

} // namespace pokke::save_crypto
