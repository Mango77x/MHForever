#pragma once

// SHA-1 (identification hashes of dumps, as listed in games/<id>/regions/*/region.toml) and SHA-256 (friend codes).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace mhf::launcher {

class Sha1 {
public:
    Sha1();
    void update(const void *data, std::size_t size);
    std::array<std::uint8_t, 20> finish();

private:
    void block(const std::uint8_t *p);
    std::uint32_t h_[5];
    std::uint8_t buf_[64];
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
};

class Sha256 {
public:
    Sha256();
    void update(const void *data, std::size_t size);
    std::array<std::uint8_t, 32> finish();

private:
    void block(const std::uint8_t *p);
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
};

std::string to_hex(const std::uint8_t *data, std::size_t size);

std::array<std::uint8_t, 32> hmac_sha256(const void *key, std::size_t key_size, const void *data, std::size_t size);
// HKDF-SHA256 (RFC 5869) with an empty salt, for 32 bytes: a key of its own for each `info` from one secret.
std::array<std::uint8_t, 32> hkdf_sha256(const void *secret, std::size_t size, const std::string &info);

} // namespace mhf::launcher
