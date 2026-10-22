#pragma once

// The PSP's save-data encryption (the firmware's sceChnnlsv), so saves made
// by pokke load in PPSSPP and on a PSP, and theirs load in pokke.
//
// An encrypted data file is a 16-byte header followed by the data XORed with
// a keystream. The keystream comes from AES with fixed PSP keys, counter
// blocks built from the header, and the game's key. A MAC of the file goes
// into the PARAM.SFO's SAVEDATA_FILE_LIST, and MACs of the PARAM.SFO itself
// into its SAVEDATA_PARAMS.
//
// Modes 1, 3 and 5 use only fixed keys and are reproduced exactly. The
// "console" variants (2, 4, 6) need each PSP's own fuse keys, which nobody
// outside a PSP has; where the scheme asks for one of those (one PARAM.SFO
// hash in mode 5), pokke fills it with 0x01 bytes as other PC tools do.
//
// This is the only decryption code in pokke (AGENTS.md); it handles save
// data and nothing else.

#include "save_crypto/aes.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pokke::save_crypto {

// The mode a save is encrypted with, from SAVEDATA_PARAMS's first byte:
// 0x4n mode 5, 0x2n mode 3, otherwise mode 1. 0 when the save is not encrypted.
int mode_from_params(std::uint8_t first);

struct EncryptedFile {
    std::vector<std::uint8_t> bytes; // header + encrypted data
    Block hash{};                    // for the SAVEDATA_FILE_LIST entry
};

// Encrypts a data file. `iv_seed` is 16 random bytes (only the first 12 are
// used); `key` is the game's save key (all zero: mode 1).
EncryptedFile encrypt(std::span<const std::uint8_t> plain, const Block &key, int mode, const Block &iv_seed);
// Decrypts a data file; nothing when it is shorter than its header.
std::optional<std::vector<std::uint8_t>> decrypt(std::span<const std::uint8_t> file, const Block &key, int mode);
// The MAC a PSP keeps for an encrypted file (also returned by encrypt).
Block file_hash(std::span<const std::uint8_t> file, const Block &key, int mode);

// Writes the file's hash into its SAVEDATA_FILE_LIST entry and fills
// SAVEDATA_PARAMS (mode flags and the PARAM.SFO hashes). `sfo` is a whole
// PARAM.SFO; false when it lacks those fields or the file's entry.
bool sign_param_sfo(std::vector<std::uint8_t> &sfo, const std::string &file_name, const Block &file_hash, int mode);

// The hash SAVEDATA_FILE_LIST keeps for `file_name` (zero when it has none).
Block stored_file_hash(std::span<const std::uint8_t> sfo, const std::string &file_name);

// SAVEDATA_PARAMS's first byte, or nothing when the PARAM.SFO has none.
std::optional<std::uint8_t> params_flags(std::span<const std::uint8_t> sfo);

} // namespace pokke::save_crypto
