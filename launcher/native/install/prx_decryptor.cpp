// Executable decryption for the installer. The cryptography is PPSSPP's PRX decrypter (third_party/ppsspp_prx, GPL);
// this file only adapts it to the Decryptor interface and rebuilds the signed-ELF layout the recompiler was generated
// from. The caller (prepare_executable) still checks the result against the known size and SHA-1 before using it.
#include "install/executable_preparation.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#include "Core/ELF/PrxDecrypter.h"

namespace mhf::launcher::install {

namespace {

constexpr std::size_t kHeaderSize = 0x150;      // size of a ~PSP header
constexpr std::size_t kElfSizeOffset = 0x28;    // plain ELF size
constexpr std::size_t kPspSizeOffset = 0x2C;    // whole ~PSP file size
constexpr std::uint8_t kSignatureMagic[4] = {0xFF, 0xEE, 0xDD, 0xCC};

std::uint32_t read_u32(const Bytes &b, std::size_t off) {
    return std::uint32_t(b[off]) | std::uint32_t(b[off + 1]) << 8 | std::uint32_t(b[off + 2]) << 16 |
           std::uint32_t(b[off + 3]) << 24;
}

std::optional<Bytes> decrypt_eboot(const Bytes &in) {
    if (in.size() < kHeaderSize + 16 || std::memcmp(in.data(), "~PSP", 4) != 0) return std::nullopt;
    const std::uint32_t psp_size = read_u32(in, kPspSizeOffset);
    const std::uint32_t elf_size = read_u32(in, kElfSizeOffset);
    if (psp_size != in.size() || elf_size == 0 || std::size_t(elf_size) + 4 > psp_size) return std::nullopt;

    std::vector<std::uint8_t> out(in.size() + 16);
    const int n = pspDecryptPRX(in.data(), out.data(), static_cast<std::uint32_t>(in.size()));
    if (n <= 0 || std::size_t(n) > out.size() || std::uint32_t(n) != elf_size) return std::nullopt;
    out.resize(std::size_t(n));

    // The recompiler's reference executable is the plain ELF followed by the signing trailer: the 4-byte marker and the
    // last (psp_size - elf_size - 4) bytes of the original file.
    const std::size_t trailer = std::size_t(psp_size) - elf_size - 4;
    out.insert(out.end(), kSignatureMagic, kSignatureMagic + 4);
    out.insert(out.end(), in.end() - static_cast<std::ptrdiff_t>(trailer), in.end());
    return out;
}

} // namespace

const Decryptor &builtin_decryptor() {
    static const Decryptor decryptor = [](const Bytes &in) { return decrypt_eboot(in); };
    return decryptor;
}

} // namespace mhf::launcher::install
