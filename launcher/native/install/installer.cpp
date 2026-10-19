#include "install/installer.hpp"

#include "install/data_dir.hpp"
#include "iso9660.hpp"

#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

namespace mhf::launcher::install {

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kMaxExecutableBytes = 64ull << 20;
constexpr std::uint64_t kSpaceMargin = 64ull << 20;

std::uint64_t megabytes(std::uint64_t bytes) { return (bytes + (1ull << 20) - 1) >> 20; }

bool is_compressed_image(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    char magic[4] = {};
    in.read(magic, 4);
    return in.gcount() == 4 && (std::memcmp(magic, "CISO", 4) == 0 || std::memcmp(magic, "ZISO", 4) == 0);
}

bool replace_file(const fs::path &from, const fs::path &to) {
    std::error_code ec;
    // rename() replaces an existing file in one step on every platform we support.
    fs::rename(from, to, ec);
    if (!ec) return true;
    // Rare fallback (e.g. a filesystem that refuses to replace): keep the old file until the new one is in place.
    fs::path backup = to;
    backup += ".old";
    fs::remove(backup, ec);
    fs::rename(to, backup, ec);
    if (ec && fs::exists(to)) return false;
    std::error_code move_ec;
    fs::rename(from, to, move_ec);
    if (move_ec) {
        if (fs::exists(backup)) fs::rename(backup, to, ec); // put the old file back
        return false;
    }
    fs::remove(backup, ec);
    return true;
}

bool write_file(const fs::path &path, const Bytes &data) {
    fs::path tmp = path;
    tmp += ".part";
    bool ok = false;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        out.flush();
        ok = static_cast<bool>(out);
    }
    std::error_code ec;
    if (!ok) {
        fs::remove(tmp, ec);
        return false;
    }
    if (!replace_file(tmp, path)) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// Copies under a temporary name so an interrupted copy is never mistaken for an image.
bool copy_image_file(const fs::path &from, const fs::path &to, std::uint64_t size, const Progress &progress) {
    fs::path tmp = to;
    tmp += ".part";
    bool ok = true;
    {
        std::ifstream in(from, std::ios::binary);
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!in || !out) {
            ok = false;
        } else {
            std::vector<char> buf(4u << 20);
            std::uint64_t done = 0;
            while (in) {
                in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                const auto n = static_cast<std::size_t>(in.gcount());
                if (n == 0) break;
                out.write(buf.data(), static_cast<std::streamsize>(n));
                if (!out) {
                    ok = false;
                    break;
                }
                done += n;
                if (progress.report && size > 0) progress.report("copying", static_cast<double>(done) / static_cast<double>(size));
            }
            out.flush();
            ok = ok && static_cast<bool>(out) && done == size;
        }
    }
    std::error_code ec;
    if (!ok || !replace_file(tmp, to)) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace

InstallResult install_from_image(const fs::path &iso, const CatalogEntry &entry, const fs::path &data_dir,
                                 const InstallOptions &options, const Decryptor &decrypt, const Progress &progress) {
    InstallResult result;
    const auto fail = [&](InstallError error, std::string message) {
        result.error = error;
        result.message = std::move(message);
        return result;
    };

    const ExecutableIdentity identity{entry.encrypted_executable_size, entry.encrypted_executable_sha1,
                                      entry.dev_executable_size, entry.dev_executable_sha1};
    if (options.prepare_executable && !identity.known()) {
        return fail(InstallError::UnknownRelease, "This release cannot be set up from a disc image yet.");
    }

    Bytes encrypted;
    if (options.prepare_executable) {
        if (is_compressed_image(iso)) {
            return fail(InstallError::CompressedImage,
                        "This is a compressed disc image (CSO/ZSO). Convert it to a plain .iso first.");
        }
        try {
            psp::IsoImage image(iso);
            const auto exe = image.find("/PSP_GAME/SYSDIR/EBOOT.BIN");
            if (!exe || exe->is_directory) {
                return fail(InstallError::ExecutableMissing,
                            "The disc image has no game executable. It may be incomplete or damaged.");
            }
            if (exe->size != identity.encrypted_size || exe->size > kMaxExecutableBytes) {
                return fail(InstallError::WrongRelease,
                            "This image is not the release MHForever supports. It may be another revision of the "
                            "game, or a modified image.");
            }
            encrypted = image.read_file(*exe, 0, exe->size);
        } catch (const std::exception &) {
            return fail(InstallError::NotAnImage,
                        "This file is not a PSP disc image. Choose the .iso you made from your own UMD.");
        }
    }

    const std::string disc_id = entry.disc_id;
    const fs::path directory = game_dir(data_dir, disc_id);
    std::error_code ec;
    bool wrote_executable = false;
    bool exe_existed_before = false;
    // A failed image copy must not leave a half-finished setup behind: drop the executable this run created.
    const auto fail_copy = [&](InstallError error, std::string message) {
        if (wrote_executable && !exe_existed_before) {
            std::error_code rm_ec;
            fs::remove(result.executable, rm_ec);
            result.executable.clear();
        }
        return fail(error, std::move(message));
    };

    if (options.prepare_executable) {
        if (progress.report) progress.report("preparing", 0);
        auto prepared = prepare_executable(encrypted, identity, decrypt);
        switch (prepared.status) {
        case PrepareStatus::Ok:
            break;
        case PrepareStatus::EncryptedMismatch:
            return fail(InstallError::WrongRelease,
                        "This image is not the release MHForever supports. It may be another revision of the game, "
                        "or a modified image.");
        case PrepareStatus::DecryptorUnavailable:
            return fail(InstallError::PreparationUnavailable,
                        "This build cannot prepare the game's executable from a disc image yet.");
        case PrepareStatus::UnknownRelease:
            return fail(InstallError::UnknownRelease, "This release cannot be set up from a disc image yet.");
        case PrepareStatus::DecryptFailed:
        case PrepareStatus::ResultMismatch:
            return fail(InstallError::PreparationFailed,
                        "The game's executable could not be prepared from this image. The image may be modified.");
        }
        fs::create_directories(directory, ec);
        const fs::path exe_path = executable_path(data_dir, disc_id);
        exe_existed_before = fs::exists(exe_path);
        if (ec || !write_file(exe_path, prepared.executable)) {
            return fail(InstallError::Io, "Could not write the game's data to " + directory.string() + ".");
        }
        result.executable = exe_path;
        wrote_executable = true;
        if (progress.report) progress.report("preparing", 1);
    }

    result.image = iso;
    if (options.copy_image) {
        const fs::path destination = image_path(data_dir, disc_id);
        fs::create_directories(directory, ec);
        const bool already_there = fs::exists(destination, ec) && fs::equivalent(destination, iso, ec);
        if (already_there) {
            result.image = destination;
        } else {
            const std::uint64_t size = fs::file_size(iso, ec);
            if (ec) return fail_copy(InstallError::Io, "The disc image could not be read.");
            const auto space = fs::space(directory, ec);
            if (!ec && space.available < size + kSpaceMargin) {
                return fail_copy(InstallError::NoSpace, "Not enough free space for a copy of the image: about " +
                                                       std::to_string(megabytes(size + kSpaceMargin)) +
                                                       " MB are needed in " + directory.string() +
                                                       ". Use the image where it is instead.");
            }
            if (!copy_image_file(iso, destination, size, progress)) {
                return fail_copy(InstallError::Io, "The disc image could not be copied to " + directory.string() + ".");
            }
            result.image = destination;
        }
    }
    return result;
}

} // namespace mhf::launcher::install
