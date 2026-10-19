#pragma once

// Prepares the game's executable from the encrypted one on the player's own disc image, so that released builds need
// nothing but the disc image. Deliberately narrow, and the only place in the project that deals with the
// executable's encryption; only the installer calls it.
//
//  * It accepts only the encrypted executable whose size and SHA-1 the region records, and refuses anything else.
//  * It refuses any result that does not match the SHA-1 of the executable the recompiled code was generated from, so
//    a wrong or partial decryption can never reach the game.
//  * The decryption itself is a separate, replaceable step (`Decryptor`) so that this gate is testable without it.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mhf::launcher::install {

using Bytes = std::vector<std::uint8_t>;

// Identification data from games/<id>/regions/*/region.toml (hashes identify a file; they contain no game content).
struct ExecutableIdentity {
    std::uint64_t encrypted_size = 0;
    std::string encrypted_sha1;
    std::uint64_t executable_size = 0;
    std::string executable_sha1;
    [[nodiscard]] bool known() const {
        return encrypted_size != 0 && !encrypted_sha1.empty() && executable_size != 0 && !executable_sha1.empty();
    }
};

// Turns the encrypted executable into the plain one, or nothing when it cannot.
using Decryptor = std::function<std::optional<Bytes>(const Bytes &encrypted)>;

enum class PrepareStatus {
    Ok,
    UnknownRelease,       // the region records no identity for the executable
    EncryptedMismatch,    // the input is not the executable this release supports
    DecryptorUnavailable, // this build ships no decryptor
    DecryptFailed,        // the decryptor gave up
    ResultMismatch,       // the decryptor's output is not the executable the code was generated from
};

struct PrepareResult {
    PrepareStatus status = PrepareStatus::UnknownRelease;
    Bytes executable; // only when status == Ok
};

PrepareResult prepare_executable(const Bytes &encrypted, const ExecutableIdentity &identity, const Decryptor &decrypt);

// The decryptor this build ships (prx_decryptor.cpp, built on third_party/ppsspp_prx).
const Decryptor &builtin_decryptor();

std::string sha1_hex(const Bytes &data);

} // namespace mhf::launcher::install
