#include "install/executable_preparation.hpp"

#include "hash.hpp"

namespace mhf::launcher::install {

std::string sha1_hex(const Bytes &data) {
    Sha1 sha;
    if (!data.empty()) sha.update(data.data(), data.size());
    const auto digest = sha.finish();
    return to_hex(digest.data(), digest.size());
}

PrepareResult prepare_executable(const Bytes &encrypted, const ExecutableIdentity &identity, const Decryptor &decrypt) {
    PrepareResult result;
    if (!identity.known()) {
        result.status = PrepareStatus::UnknownRelease;
        return result;
    }
    if (encrypted.size() != identity.encrypted_size || sha1_hex(encrypted) != identity.encrypted_sha1) {
        result.status = PrepareStatus::EncryptedMismatch;
        return result;
    }
    if (!decrypt) {
        result.status = PrepareStatus::DecryptorUnavailable;
        return result;
    }
    auto plain = decrypt(encrypted);
    if (!plain) {
        result.status = PrepareStatus::DecryptFailed;
        return result;
    }
    if (plain->size() != identity.executable_size || sha1_hex(*plain) != identity.executable_sha1) {
        result.status = PrepareStatus::ResultMismatch;
        return result;
    }
    result.status = PrepareStatus::Ok;
    result.executable = std::move(*plain);
    return result;
}

} // namespace mhf::launcher::install
