// Installer tests on a synthetic disc image and a fake decryptor: no game data, safe for CI.

#include "install/data_dir.hpp"
#include "install/executable_preparation.hpp"
#include "install/installer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace mhf::launcher;
using namespace mhf::launcher::install;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

constexpr std::uint32_t kSector = 2048;

void put_both32(std::uint8_t *p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
        p[7 - i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

std::size_t put_record(std::uint8_t *p, std::uint32_t lba, std::uint32_t size, bool dir, const std::string &name) {
    const std::size_t length = (33 + name.size() + 1) & ~std::size_t{1};
    p[0] = static_cast<std::uint8_t>(length);
    put_both32(p + 2, lba);
    put_both32(p + 10, size);
    p[25] = dir ? 0x02 : 0x00;
    p[32] = static_cast<std::uint8_t>(name.size());
    std::memcpy(p + 33, name.data(), name.size());
    return length;
}

// A tiny image holding only /PSP_GAME/SYSDIR/EBOOT.BIN with the given content.
Bytes build_image(const Bytes &eboot) {
    const std::uint32_t sectors = static_cast<std::uint32_t>((eboot.size() + kSector - 1) / kSector);
    Bytes image((24 + sectors) * kSector, 0);
    std::uint8_t *pvd = &image[16 * kSector];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    put_record(pvd + 156, 20, kSector, true, std::string(1, '\0'));
    auto dir = [&](std::uint32_t lba, std::uint32_t parent, const std::string &child, std::uint32_t child_lba,
                   std::uint32_t child_size, bool child_dir) {
        std::uint8_t *p = &image[lba * kSector];
        p += put_record(p, lba, kSector, true, std::string(1, '\0'));
        p += put_record(p, parent, kSector, true, std::string(1, '\1'));
        put_record(p, child_lba, child_size, child_dir, child);
    };
    dir(20, 20, "PSP_GAME", 21, kSector, true);
    dir(21, 20, "SYSDIR", 22, kSector, true);
    dir(22, 21, "EBOOT.BIN;1", 23, static_cast<std::uint32_t>(eboot.size()), false);
    std::memcpy(&image[23 * kSector], eboot.data(), eboot.size());
    return image;
}

void write(const fs::path &path, const Bytes &data) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
}

Bytes read(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

Bytes pattern(std::size_t size, std::uint8_t seed) {
    Bytes b(size);
    for (std::size_t i = 0; i < size; ++i) b[i] = static_cast<std::uint8_t>(seed + i * 7);
    return b;
}

std::size_t count_files(const fs::path &dir) {
    std::size_t n = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return 0;
    for (const auto &e : fs::recursive_directory_iterator(dir, ec)) n += e.is_regular_file() ? 1 : 0;
    return n;
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "mhf_install_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    // Per-user data directory: the override wins, and the paths are per game.
#if defined(_WIN32)
    _putenv_s("MHF_DATA_DIR", (root / "override").string().c_str());
#else
    setenv("MHF_DATA_DIR", (root / "override").c_str(), 1);
#endif
    CHECK(user_data_dir() == root / "override");
    CHECK(executable_path(root, "ABCD01234") == root / "games" / "ABCD01234" / "executable.elf");
    CHECK(image_path(root, "ABCD01234") == root / "games" / "ABCD01234" / "image.iso");

    // The fake "release": an encrypted executable, the plain one it prepares to, and their identities.
    const Bytes encrypted = pattern(5000, 3);
    const Bytes plain = pattern(3000, 90);
    const std::string encrypted_sha1 = sha1_hex(encrypted);
    const std::string plain_sha1 = sha1_hex(plain);
    const Decryptor good = [&](const Bytes &in) -> std::optional<Bytes> { return in == encrypted ? std::optional<Bytes>(plain) : std::nullopt; };

    // prepare_executable: every gate.
    const ExecutableIdentity identity{encrypted.size(), encrypted_sha1, plain.size(), plain_sha1};
    CHECK(prepare_executable(encrypted, identity, good).status == PrepareStatus::Ok);
    CHECK(prepare_executable(encrypted, identity, good).executable == plain);
    CHECK(prepare_executable(encrypted, ExecutableIdentity{}, good).status == PrepareStatus::UnknownRelease);
    Bytes altered = encrypted;
    altered[100] ^= 1;
    CHECK(prepare_executable(altered, identity, good).status == PrepareStatus::EncryptedMismatch);
    CHECK(prepare_executable(encrypted, identity, Decryptor{}).status == PrepareStatus::DecryptorUnavailable);
    CHECK(prepare_executable(encrypted, identity, [](const Bytes &) { return std::optional<Bytes>(); }).status == PrepareStatus::DecryptFailed);
    Bytes wrong = plain;
    wrong[0] ^= 1;
    CHECK(prepare_executable(encrypted, identity, [&](const Bytes &) { return std::optional<Bytes>(wrong); }).status == PrepareStatus::ResultMismatch);
    // The built-in decryptor refuses anything that is not a genuine ~PSP executable.
    CHECK(builtin_decryptor());
    CHECK(!builtin_decryptor()(Bytes(4096, 0x41)));
    CHECK(!builtin_decryptor()(encrypted));

    // install_from_image on synthetic images.
    CatalogEntry entry{"test", "Test", "TEST00001", "1.0", "EU", "in-development", {}, plain.size(), plain_sha1.c_str(),
                       encrypted.size(), encrypted_sha1.c_str()};
    const fs::path iso = root / "game.iso";
    write(iso, build_image(encrypted));
    const Progress quiet{};

    {   // in place: executable prepared, image used where it is
        const fs::path data = root / "data_in_place";
        auto r = install_from_image(iso, entry, data, InstallOptions{true, false}, good, quiet);
        CHECK(r.error == InstallError::None);
        CHECK(r.image == iso);
        CHECK(read(executable_path(data, "TEST00001")) == plain);
        CHECK(!fs::exists(image_path(data, "TEST00001")));
        CHECK(count_files(data) == 1); // no leftover temporary files
    }
    {   // copy: image kept in the data directory, byte for byte, with progress
        const fs::path data = root / "data_copy";
        double last = -1;
        std::string phases;
        Progress progress{[&](const char *phase, double fraction) {
            if (phases.find(phase) == std::string::npos) phases += std::string(phase) + ",";
            last = fraction;
        }};
        auto r = install_from_image(iso, entry, data, InstallOptions{true, true}, good, progress);
        CHECK(r.error == InstallError::None);
        CHECK(r.image == image_path(data, "TEST00001"));
        CHECK(read(r.image) == read(iso));
        CHECK(read(executable_path(data, "TEST00001")) == plain);
        CHECK(phases == "preparing,copying,");
        CHECK(last == 1.0);
        CHECK(count_files(data) == 2);
        // Installing again from the copy itself is a no-op for the image.
        auto again = install_from_image(r.image, entry, data, InstallOptions{true, true}, good, quiet);
        CHECK(again.error == InstallError::None && again.image == r.image);
    }
    {   // another revision (different executable): refused, nothing written
        Bytes other = encrypted;
        other[2000] ^= 0xFF;
        const fs::path other_iso = root / "other.iso";
        write(other_iso, build_image(other));
        const fs::path data = root / "data_other";
        auto r = install_from_image(other_iso, entry, data, InstallOptions{true, true}, good, quiet);
        CHECK(r.error == InstallError::WrongRelease);
        CHECK(!r.message.empty());
        CHECK(!fs::exists(data));
    }
    {   // wrong size: refused before anything is read into memory
        const fs::path short_iso = root / "short.iso";
        write(short_iso, build_image(pattern(1234, 5)));
        auto r = install_from_image(short_iso, entry, root / "data_short", InstallOptions{true, false}, good, quiet);
        CHECK(r.error == InstallError::WrongRelease);
    }
    {   // no decryptor in this build: clear error, nothing written
        const fs::path data = root / "data_nodecrypt";
        auto r = install_from_image(iso, entry, data, InstallOptions{true, true}, Decryptor{}, quiet);
        CHECK(r.error == InstallError::PreparationUnavailable);
        CHECK(!fs::exists(data));
    }
    {   // a decryptor that produces the wrong file: refused
        const fs::path data = root / "data_wrong";
        auto r = install_from_image(iso, entry, data, InstallOptions{true, false}, [&](const Bytes &) { return std::optional<Bytes>(wrong); }, quiet);
        CHECK(r.error == InstallError::PreparationFailed);
        CHECK(!fs::exists(data));
    }
    {   // compressed image and not-an-image
        const fs::path ciso = root / "game.cso";
        Bytes c(4096, 0);
        std::memcpy(c.data(), "CISO", 4);
        write(ciso, c);
        CHECK(install_from_image(ciso, entry, root / "d1", InstallOptions{true, false}, good, quiet).error == InstallError::CompressedImage);
        const fs::path text = root / "notes.txt";
        write(text, Bytes(100000, 'x'));
        CHECK(install_from_image(text, entry, root / "d2", InstallOptions{true, false}, good, quiet).error == InstallError::NotAnImage);
        CHECK(install_from_image(root / "missing.iso", entry, root / "d3", InstallOptions{true, false}, good, quiet).error == InstallError::NotAnImage);
    }
    {   // an unidentified release cannot be prepared, but its image can still be kept (developer builds)
        CatalogEntry bare{"test", "Test", "TEST00002", "1.0", "EU", "planned", {}, 0, "", 0, ""};
        CHECK(install_from_image(iso, bare, root / "d4", InstallOptions{true, false}, good, quiet).error == InstallError::UnknownRelease);
        auto kept = install_from_image(iso, bare, root / "d5", InstallOptions{false, true}, Decryptor{}, quiet);
        CHECK(kept.error == InstallError::None && read(kept.image) == read(iso));
    }

    fs::remove_all(root, ec);
    if (failures == 0) std::puts("install tests passed");
    return failures == 0 ? 0 : 1;
}
