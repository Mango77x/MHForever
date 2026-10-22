#pragma once

// AES-128 (FIPS-197) block cipher and AES-CMAC (RFC 4493), as the PSP's
// save-data scheme needs them. Written for pokke; checked against the
// standards' test vectors in tests/save_crypto_tests.cpp.

#include <array>
#include <cstdint>
#include <span>

namespace pokke::save_crypto {

using Block = std::array<std::uint8_t, 16>;

class Aes128 {
public:
    explicit Aes128(const Block &key);
    Block encrypt(const Block &in) const;
    Block decrypt(const Block &in) const;

private:
    std::array<std::array<std::uint8_t, 16>, 11> round_keys_{};
};

// AES-CMAC of `message` under `key`.
Block cmac(const Block &key, std::span<const std::uint8_t> message);

Block xor_blocks(const Block &a, const Block &b);

} // namespace pokke::save_crypto
