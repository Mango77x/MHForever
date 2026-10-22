#include "save_crypto/aes.hpp"

#include <algorithm>

namespace pokke::save_crypto {

namespace {

std::uint8_t xtime(std::uint8_t x) { return static_cast<std::uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

std::uint8_t multiply(std::uint8_t a, std::uint8_t b) {
    std::uint8_t product = 0;
    while (b != 0) {
        if (b & 1) product ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return product;
}

// The S-box from its definition: the multiplicative inverse in GF(2^8),
// then the affine transform.
struct Tables {
    std::array<std::uint8_t, 256> sbox{}, inverse{};
    Tables() {
        for (int x = 0; x < 256; ++x) {
            std::uint8_t inv = 0;
            if (x != 0)
                for (int y = 1; y < 256; ++y)
                    if (multiply(static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)) == 1) {
                        inv = static_cast<std::uint8_t>(y);
                        break;
                    }
            std::uint8_t s = inv;
            for (int shift = 1; shift <= 4; ++shift)
                s ^= static_cast<std::uint8_t>((inv << shift) | (inv >> (8 - shift)));
            s ^= 0x63;
            sbox[static_cast<std::size_t>(x)] = s;
            inverse[s] = static_cast<std::uint8_t>(x);
        }
    }
};

const Tables &tables() {
    static const Tables t;
    return t;
}

using State = std::array<std::uint8_t, 16>; // column-major, as FIPS-197 lays it out

void add_round_key(State &s, const std::array<std::uint8_t, 16> &k) {
    for (int i = 0; i < 16; ++i) s[static_cast<std::size_t>(i)] ^= k[static_cast<std::size_t>(i)];
}

void shift_rows(State &s, bool inverse) {
    State t = s;
    for (int r = 1; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const int from = inverse ? (c - r + 4) % 4 : (c + r) % 4;
            s[static_cast<std::size_t>(c * 4 + r)] = t[static_cast<std::size_t>(from * 4 + r)];
        }
}

void mix_columns(State &s, bool inverse) {
    for (int c = 0; c < 4; ++c) {
        std::uint8_t *col = &s[static_cast<std::size_t>(c * 4)];
        const std::uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        if (!inverse) {
            col[0] = static_cast<std::uint8_t>(multiply(a0, 2) ^ multiply(a1, 3) ^ a2 ^ a3);
            col[1] = static_cast<std::uint8_t>(a0 ^ multiply(a1, 2) ^ multiply(a2, 3) ^ a3);
            col[2] = static_cast<std::uint8_t>(a0 ^ a1 ^ multiply(a2, 2) ^ multiply(a3, 3));
            col[3] = static_cast<std::uint8_t>(multiply(a0, 3) ^ a1 ^ a2 ^ multiply(a3, 2));
        } else {
            col[0] = static_cast<std::uint8_t>(multiply(a0, 14) ^ multiply(a1, 11) ^ multiply(a2, 13) ^ multiply(a3, 9));
            col[1] = static_cast<std::uint8_t>(multiply(a0, 9) ^ multiply(a1, 14) ^ multiply(a2, 11) ^ multiply(a3, 13));
            col[2] = static_cast<std::uint8_t>(multiply(a0, 13) ^ multiply(a1, 9) ^ multiply(a2, 14) ^ multiply(a3, 11));
            col[3] = static_cast<std::uint8_t>(multiply(a0, 11) ^ multiply(a1, 13) ^ multiply(a2, 9) ^ multiply(a3, 14));
        }
    }
}

} // namespace

Aes128::Aes128(const Block &key) {
    const auto &t = tables();
    std::array<std::uint8_t, 176> w{};
    for (int i = 0; i < 16; ++i) w[static_cast<std::size_t>(i)] = key[static_cast<std::size_t>(i)];
    std::uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        std::uint8_t temp[4] = {w[static_cast<std::size_t>(i - 4)], w[static_cast<std::size_t>(i - 3)],
                                w[static_cast<std::size_t>(i - 2)], w[static_cast<std::size_t>(i - 1)]};
        if (i % 16 == 0) {
            const std::uint8_t first = temp[0];
            temp[0] = static_cast<std::uint8_t>(t.sbox[temp[1]] ^ rcon);
            temp[1] = t.sbox[temp[2]];
            temp[2] = t.sbox[temp[3]];
            temp[3] = t.sbox[first];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; ++j)
            w[static_cast<std::size_t>(i + j)] = static_cast<std::uint8_t>(w[static_cast<std::size_t>(i + j - 16)] ^ temp[j]);
    }
    for (int r = 0; r < 11; ++r)
        for (int i = 0; i < 16; ++i)
            round_keys_[static_cast<std::size_t>(r)][static_cast<std::size_t>(i)] = w[static_cast<std::size_t>(r * 16 + i)];
}

Block Aes128::encrypt(const Block &in) const {
    const auto &t = tables();
    State s = in;
    add_round_key(s, round_keys_[0]);
    for (int round = 1; round <= 10; ++round) {
        for (auto &b : s) b = t.sbox[b];
        shift_rows(s, false);
        if (round != 10) mix_columns(s, false);
        add_round_key(s, round_keys_[static_cast<std::size_t>(round)]);
    }
    return s;
}

Block Aes128::decrypt(const Block &in) const {
    const auto &t = tables();
    State s = in;
    add_round_key(s, round_keys_[10]);
    for (int round = 9; round >= 0; --round) {
        shift_rows(s, true);
        for (auto &b : s) b = t.inverse[b];
        add_round_key(s, round_keys_[static_cast<std::size_t>(round)]);
        if (round != 0) mix_columns(s, true);
    }
    return s;
}

Block xor_blocks(const Block &a, const Block &b) {
    Block out{};
    for (std::size_t i = 0; i < 16; ++i) out[i] = static_cast<std::uint8_t>(a[i] ^ b[i]);
    return out;
}

namespace {

// Doubling in GF(2^128), the CMAC subkey step.
Block twice(const Block &b) {
    Block out{};
    for (std::size_t i = 0; i < 16; ++i)
        out[i] = static_cast<std::uint8_t>((b[i] << 1) | (i + 1 < 16 ? b[i + 1] >> 7 : 0));
    if (b[0] & 0x80) out[15] ^= 0x87;
    return out;
}

} // namespace

Block cmac(const Block &key, std::span<const std::uint8_t> message) {
    const Aes128 aes(key);
    const Block k1 = twice(aes.encrypt(Block{}));
    const Block k2 = twice(k1);
    const std::size_t blocks = message.empty() ? 1 : (message.size() + 15) / 16;
    const bool complete = !message.empty() && message.size() % 16 == 0;
    Block chain{};
    for (std::size_t n = 0; n < blocks; ++n) {
        Block block{};
        const std::size_t start = n * 16;
        const std::size_t take = std::min<std::size_t>(16, message.size() > start ? message.size() - start : 0);
        for (std::size_t i = 0; i < take; ++i) block[i] = message[start + i];
        if (n + 1 == blocks) {
            if (complete) {
                block = xor_blocks(block, k1);
            } else {
                block[take] = 0x80;
                block = xor_blocks(block, k2);
            }
        }
        chain = aes.encrypt(xor_blocks(chain, block));
    }
    return chain;
}

} // namespace pokke::save_crypto
