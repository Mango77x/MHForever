#include "hash.hpp"

#include <algorithm>
#include <cstring>

namespace mhf::launcher {

namespace {
constexpr std::uint32_t rol(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
constexpr std::uint32_t ror(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
std::uint32_t be32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}
} // namespace

// ------------------------------------------------------------------ SHA-1 (FIPS 180-4)

Sha1::Sha1() : h_{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u}, buf_{} {}

void Sha1::block(const std::uint8_t *p) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = be32(p + i * 4);
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
}

void Sha1::update(const void *data, std::size_t size) {
    const auto *p = static_cast<const std::uint8_t *>(data);
    total_ += size;
    if (used_ > 0) {
        const std::size_t take = std::min(size, 64 - used_);
        std::memcpy(buf_ + used_, p, take);
        used_ += take; p += take; size -= take;
        if (used_ < 64) return;
        block(buf_);
        used_ = 0;
    }
    while (size >= 64) { block(p); p += 64; size -= 64; }
    std::memcpy(buf_, p, size);
    used_ = size;
}

std::array<std::uint8_t, 20> Sha1::finish() {
    const std::uint64_t bits = total_ * 8;
    const std::uint8_t pad = 0x80, zero = 0;
    update(&pad, 1);
    while (used_ != 56) update(&zero, 1);
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<std::uint8_t>(bits >> (56 - i * 8));
    update(len, 8);
    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i) for (int j = 0; j < 4; ++j) out[static_cast<std::size_t>(i * 4 + j)] = static_cast<std::uint8_t>(h_[i] >> (24 - j * 8));
    return out;
}

// ------------------------------------------------------------------ SHA-256 (FIPS 180-4)

namespace {
constexpr std::uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
}

Sha256::Sha256()
    : h_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u}, buf_{} {}

void Sha256::block(const std::uint8_t *p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = be32(p + i * 4);
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + K256[i] + w[i];
        const std::uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void *data, std::size_t size) {
    const auto *p = static_cast<const std::uint8_t *>(data);
    total_ += size;
    if (used_ > 0) {
        const std::size_t take = std::min(size, 64 - used_);
        std::memcpy(buf_ + used_, p, take);
        used_ += take; p += take; size -= take;
        if (used_ < 64) return;
        block(buf_);
        used_ = 0;
    }
    while (size >= 64) { block(p); p += 64; size -= 64; }
    std::memcpy(buf_, p, size);
    used_ = size;
}

std::array<std::uint8_t, 32> Sha256::finish() {
    const std::uint64_t bits = total_ * 8;
    const std::uint8_t pad = 0x80, zero = 0;
    update(&pad, 1);
    while (used_ != 56) update(&zero, 1);
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<std::uint8_t>(bits >> (56 - i * 8));
    update(len, 8);
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) for (int j = 0; j < 4; ++j) out[static_cast<std::size_t>(i * 4 + j)] = static_cast<std::uint8_t>(h_[i] >> (24 - j * 8));
    return out;
}

std::array<std::uint8_t, 32> hmac_sha256(const void *key, std::size_t key_size, const void *data, std::size_t size) {
    std::uint8_t block[64] = {};
    if (key_size > sizeof block) {
        Sha256 k;
        k.update(key, key_size);
        const auto digest = k.finish();
        std::copy(digest.begin(), digest.end(), block);
    } else if (key_size > 0) {
        std::memcpy(block, key, key_size);
    }
    std::uint8_t inner_pad[64], outer_pad[64];
    for (std::size_t i = 0; i < sizeof block; ++i) {
        inner_pad[i] = static_cast<std::uint8_t>(block[i] ^ 0x36);
        outer_pad[i] = static_cast<std::uint8_t>(block[i] ^ 0x5c);
    }
    Sha256 inner;
    inner.update(inner_pad, sizeof inner_pad);
    inner.update(data, size);
    const auto inner_digest = inner.finish();
    Sha256 outer;
    outer.update(outer_pad, sizeof outer_pad);
    outer.update(inner_digest.data(), inner_digest.size());
    return outer.finish();
}

std::array<std::uint8_t, 32> hkdf_sha256(const void *secret, std::size_t size, const std::string &info) {
    const std::uint8_t zeros[32] = {};
    const auto prk = hmac_sha256(zeros, sizeof zeros, secret, size); // extract, with the RFC's default salt
    std::string expand = info;
    expand.push_back('\x01');
    return hmac_sha256(prk.data(), prk.size(), expand.data(), expand.size()); // the first block is all 32 bytes
}

std::string to_hex(const std::uint8_t *data, std::size_t size) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
    return out;
}

} // namespace mhf::launcher
