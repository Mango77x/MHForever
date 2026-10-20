// IoFileMgrForUser and StdioForUser over host folders (docs/05-runtime-hle.md).
//
// Devices: "ms0:" (memory stick) → options.memstick_root, "disc0:"/"umd0:"/"umd1:" → options.disc_root,
// "host0:" → options.host_root. Asynchronous calls complete immediately; their result is kept for WaitAsync/PollAsync.

#include "hle.hpp"
#include "kernel.hpp"

#include "iso9660.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace mhf::psp::hle {

namespace {

using mhf::psp::IsoImage;

constexpr std::uint32_t kOpenRead = 0x0001u;
constexpr std::uint32_t kOpenWrite = 0x0002u;
constexpr std::uint32_t kOpenAppend = 0x0100u;
constexpr std::uint32_t kOpenCreate = 0x0200u;
constexpr std::uint32_t kOpenTruncate = 0x0400u;
constexpr std::uint32_t kOpenExclusive = 0x0800u;
constexpr int kFirstFd = 3;
constexpr int kMaxFds = 64;

struct OpenFile {
    std::FILE *file{};       // host file, or null for a file inside the disc image
    std::string psp_path;
    std::int64_t async_result{};
    bool async_pending{};
    // Disc image files: byte range inside the image and the current position.
    std::uint64_t image_offset{};
    std::uint64_t image_size{};
    std::uint64_t position{};
};

struct OpenDir {
    std::vector<fs::directory_entry> entries;
    std::size_t next{};
};

struct DiscDir {
    std::vector<IsoImage::Entry> entries;
    std::size_t next{};
};

struct IoState {
    std::map<int, OpenFile> files;
    std::map<int, OpenDir> dirs;
    std::map<int, DiscDir> disc_dirs;
    std::string cwd = "disc0:/PSP_GAME/USRDIR";
    std::unique_ptr<IsoImage> disc;
    bool disc_checked{};
};

std::unordered_map<const Runtime *, IoState> g_states;
IoState &state(Runtime &rt) {
    auto &s = g_states[&rt];
    if (!s.disc_checked) {
        s.disc_checked = true;
        if (!rt.options().disc_image.empty()) {
            s.disc = std::make_unique<IsoImage>(rt.options().disc_image);
        }
    }
    return s;
}

// Splits "device:/rest" (relative paths are resolved against the current directory).
std::pair<std::string, std::string> split_device(Runtime &rt, const std::string &psp_path) {
    std::string path = psp_path;
    if (path.find(':') == std::string::npos) {
        path = state(rt).cwd + "/" + path;
    }
    const auto colon = path.find(':');
    std::string device;
    for (const char c : path.substr(0, colon)) device.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    std::string rest = path.substr(colon + 1);
    // Collapse "//", "/./" and resolve "..".
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= rest.size()) {
        const auto slash = rest.find_first_of("/\\", start);
        const std::string part = rest.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string normalized;
    for (const auto &p : parts) normalized += "/" + p;
    return {device, normalized.empty() ? "/" : normalized};
}

bool is_disc_device(const std::string &device) {
    return device == "disc0" || device == "umd0" || device == "umd1" || device == "umd";
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Maps a PSP path to a host path. Returns nothing for unknown devices or paths escaping the device root.
std::optional<fs::path> host_path(Runtime &rt, const std::string &psp_path) {
    std::string path = psp_path;
    if (path.find(':') == std::string::npos) {
        path = state(rt).cwd + "/" + path;
    }
    const auto colon = path.find(':');
    const std::string device = lower(path.substr(0, colon));
    std::string rest = path.substr(colon + 1);
    const auto &o = rt.options();
    fs::path root;
    if (device == "ms0" || device == "fatms0") root = o.memstick_root;
    else if (device == "disc0" || device == "umd0" || device == "umd1") root = o.disc_root;
    else if (device == "host0") root = o.host_root;
    else return std::nullopt;
    return hle::resolve_under_root(root, rest);
}

int allocate_fd(IoState &s) {
    for (int fd = kFirstFd; fd < kMaxFds; ++fd) {
        if (!s.files.contains(fd) && !s.dirs.contains(fd) && !s.disc_dirs.contains(fd)) return fd;
    }
    return -1;
}

std::uint32_t open_on_disc(Runtime &, IoState &s, const std::string &path, const std::string &inner,
                           std::uint32_t flags) {
    if ((flags & kOpenWrite) != 0) return 0x8001001Eu; // read-only file system
    OpenFile f;
    f.psp_path = path;
    // Raw sector access: "/sce_lbn0x<lba>_size0x<bytes>".
    unsigned lba = 0, size = 0;
    if (std::sscanf(inner.c_str(), "/sce_lbn0x%x_size0x%x", &lba, &size) == 2 ||
        std::sscanf(inner.c_str(), "/sce_lbn%u_size%u", &lba, &size) == 2) {
        f.image_offset = static_cast<std::uint64_t>(lba) * IsoImage::kSectorSize;
        f.image_size = size;
    } else {
        const auto entry = s.disc->find(inner);
        if (!entry || entry->is_directory) return error::kIoFileNotFound;
        f.image_offset = static_cast<std::uint64_t>(entry->lba) * IsoImage::kSectorSize;
        f.image_size = entry->size;
    }
    const int fd = allocate_fd(s);
    if (fd < 0) return error::kIoTooManyOpen;
    s.files[fd] = std::move(f);
    return static_cast<std::uint32_t>(fd);
}

std::uint32_t do_open(Runtime &rt, const std::string &path, std::uint32_t flags) {
    {
        auto &s = state(rt);
        const auto [device, inner] = split_device(rt, path);
        if (s.disc && is_disc_device(device)) {
            return open_on_disc(rt, s, path, inner, flags);
        }
    }
    const auto host = host_path(rt, path);
    if (!host) return error::kIoNoDevice;
    std::error_code ec;
    const bool exists = fs::exists(*host, ec);
    if ((flags & kOpenExclusive) != 0 && (flags & kOpenCreate) != 0 && exists) return 0x80010011u; // EEXIST
    if (!exists && (flags & kOpenCreate) == 0) return error::kIoFileNotFound;
    if (fs::is_directory(*host, ec)) return 0x80010015u; // EISDIR
    const char *mode = "rb";
    if ((flags & kOpenWrite) != 0) {
        if ((flags & kOpenAppend) != 0) mode = (flags & kOpenRead) != 0 ? "a+b" : "ab";
        else if ((flags & kOpenTruncate) != 0 || !exists) mode = (flags & kOpenRead) != 0 ? "w+b" : "wb";
        else mode = "r+b";
    }
    std::FILE *f = std::fopen(host->string().c_str(), mode);
    if (f == nullptr) return error::kIoFileNotFound;
    auto &s = state(rt);
    const int fd = allocate_fd(s);
    if (fd < 0) {
        std::fclose(f);
        return error::kIoTooManyOpen;
    }
    s.files[fd] = OpenFile{f, path, 0, false};
    return static_cast<std::uint32_t>(fd);
}

OpenFile *file(Runtime &rt, int fd) {
    auto &s = state(rt);
    const auto it = s.files.find(fd);
    return it != s.files.end() ? &it->second : nullptr;
}

std::int64_t do_read(Runtime &rt, int fd, std::uint32_t address, std::uint32_t size) {
    if (fd == 0) return 0;
    OpenFile *f = file(rt, fd);
    if (f == nullptr) return static_cast<std::int32_t>(error::kIoBadFile);
    if (f->file == nullptr) {
        const std::uint64_t left = f->position < f->image_size ? f->image_size - f->position : 0;
        const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(left, size));
        if (n != 0) {
            const auto bytes = state(rt).disc->read_bytes(f->image_offset + f->position, n);
            rt.memory().copy_in(address, bytes);
            f->position += n;
        }
        return n;
    }
    std::vector<std::uint8_t> buffer(size);
    const std::size_t n = std::fread(buffer.data(), 1, size, f->file);
    rt.memory().copy_in(address, std::span(buffer.data(), n));
    return static_cast<std::int64_t>(n);
}

std::int64_t do_write(Runtime &rt, int fd, std::uint32_t address, std::uint32_t size) {
    std::vector<std::uint8_t> buffer(size);
    rt.memory().copy_out(address, buffer);
    if (fd == 1 || fd == 2) {
        std::FILE *out = fd == 1 ? stdout : stderr;
        std::fwrite(buffer.data(), 1, size, out);
        std::fflush(out);
        return size;
    }
    OpenFile *f = file(rt, fd);
    if (f == nullptr) return static_cast<std::int32_t>(error::kIoBadFile);
    if (f->file == nullptr) return static_cast<std::int32_t>(0x8001001Eu);
    return static_cast<std::int64_t>(std::fwrite(buffer.data(), 1, size, f->file));
}

std::int64_t do_seek(Runtime &rt, int fd, std::int64_t offset, int whence) {
    OpenFile *f = file(rt, fd);
    if (f == nullptr) return static_cast<std::int32_t>(error::kIoBadFile);
    if (f->file == nullptr) {
        const std::int64_t base = whence == 0 ? 0 : whence == 1 ? static_cast<std::int64_t>(f->position)
                                                                : static_cast<std::int64_t>(f->image_size);
        if (base + offset < 0) return static_cast<std::int32_t>(error::kIoInvalidArgument);
        f->position = static_cast<std::uint64_t>(base + offset);
        return static_cast<std::int64_t>(f->position);
    }
    const int origin = whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END;
#if defined(_WIN32)
    if (_fseeki64(f->file, offset, origin) != 0) return static_cast<std::int32_t>(error::kIoInvalidArgument);
    return _ftelli64(f->file);
#else
    if (fseeko(f->file, offset, origin) != 0) return static_cast<std::int32_t>(error::kIoInvalidArgument);
    return ftello(f->file);
#endif
}

void write_datetime(std::vector<std::uint8_t> &out, std::size_t offset, std::time_t t) {
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    const std::uint16_t fields16[] = {static_cast<std::uint16_t>(tm.tm_year + 1900),
                                      static_cast<std::uint16_t>(tm.tm_mon + 1),
                                      static_cast<std::uint16_t>(tm.tm_mday),
                                      static_cast<std::uint16_t>(tm.tm_hour),
                                      static_cast<std::uint16_t>(tm.tm_min),
                                      static_cast<std::uint16_t>(tm.tm_sec)};
    std::memcpy(out.data() + offset, fields16, sizeof fields16);
}

// SceIoStat (0x58 bytes).
std::vector<std::uint8_t> make_stat(const fs::path &p) {
    std::vector<std::uint8_t> out(0x58, 0);
    std::error_code ec;
    const bool dir = fs::is_directory(p, ec);
    const std::uint32_t mode = dir ? 0x11FFu : 0x21FFu;
    const std::uint32_t attr = dir ? 0x10u : 0x20u;
    const std::uint64_t size = dir ? 0u : static_cast<std::uint64_t>(fs::file_size(p, ec));
    std::memcpy(out.data() + 0, &mode, 4);
    std::memcpy(out.data() + 4, &attr, 4);
    std::memcpy(out.data() + 8, &size, 8);
    const auto ftime = fs::last_write_time(p, ec);
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t t = std::chrono::system_clock::to_time_t(sys);
    write_datetime(out, 0x10, t);
    write_datetime(out, 0x20, t);
    write_datetime(out, 0x30, t);
    return out;
}

// ---------------------------------------------------------------- handlers

void IoOpen(Runtime &rt, Context &ctx) { ret(ctx, do_open(rt, read_string(rt, arg(ctx, 0)), arg(ctx, 1))); }

void IoOpenAsync(Runtime &rt, Context &ctx) {
    const std::uint32_t r = do_open(rt, read_string(rt, arg(ctx, 0)), arg(ctx, 1));
    if (static_cast<std::int32_t>(r) >= 0) {
        file(rt, static_cast<int>(r))->async_result = static_cast<std::int32_t>(r);
        file(rt, static_cast<int>(r))->async_pending = true;
    }
    ret(ctx, r);
}

void IoClose(Runtime &rt, Context &ctx) {
    auto &s = state(rt);
    const int fd = sarg(ctx, 0);
    const auto it = s.files.find(fd);
    if (it == s.files.end()) {
        ret(ctx, error::kIoBadFile);
        return;
    }
    if (it->second.file != nullptr) std::fclose(it->second.file);
    s.files.erase(it);
    ret(ctx, 0);
}

void IoCloseAsync(Runtime &rt, Context &ctx) {
    // The descriptor stays valid until WaitAsync collects the result.
    OpenFile *f = file(rt, sarg(ctx, 0));
    if (f == nullptr) {
        ret(ctx, error::kIoBadFile);
        return;
    }
    if (f->file != nullptr) std::fflush(f->file);
    f->async_result = 0;
    f->async_pending = true;
    f->psp_path = "\x01close";
    ret(ctx, 0);
}

void IoRead(Runtime &rt, Context &ctx) {
    ret(ctx, static_cast<std::uint32_t>(do_read(rt, sarg(ctx, 0), arg(ctx, 1), arg(ctx, 2))));
}

void IoReadAsync(Runtime &rt, Context &ctx) {
    OpenFile *f = file(rt, sarg(ctx, 0));
    if (f == nullptr) {
        ret(ctx, error::kIoBadFile);
        return;
    }
    f->async_result = do_read(rt, sarg(ctx, 0), arg(ctx, 1), arg(ctx, 2));
    f->async_pending = true;
    ret(ctx, 0);
}

void IoWrite(Runtime &rt, Context &ctx) {
    ret(ctx, static_cast<std::uint32_t>(do_write(rt, sarg(ctx, 0), arg(ctx, 1), arg(ctx, 2))));
}

void IoWriteAsync(Runtime &rt, Context &ctx) {
    OpenFile *f = file(rt, sarg(ctx, 0));
    if (f == nullptr) {
        ret(ctx, error::kIoBadFile);
        return;
    }
    f->async_result = do_write(rt, sarg(ctx, 0), arg(ctx, 1), arg(ctx, 2));
    f->async_pending = true;
    ret(ctx, 0);
}

// 64-bit offsets arrive in an aligned register pair: (a2, a3), whence in t0.
void IoLseek(Runtime &rt, Context &ctx) {
    const std::int64_t offset = static_cast<std::int64_t>(arg(ctx, 2) | (static_cast<std::uint64_t>(arg(ctx, 3)) << 32u));
    ret64(ctx, static_cast<std::uint64_t>(do_seek(rt, sarg(ctx, 0), offset, sarg(ctx, 4))));
}

void IoLseekAsync(Runtime &rt, Context &ctx) {
    OpenFile *f = file(rt, sarg(ctx, 0));
    if (f == nullptr) {
        ret(ctx, error::kIoBadFile);
        return;
    }
    const std::int64_t offset = static_cast<std::int64_t>(arg(ctx, 2) | (static_cast<std::uint64_t>(arg(ctx, 3)) << 32u));
    f->async_result = do_seek(rt, sarg(ctx, 0), offset, sarg(ctx, 4));
    f->async_pending = true;
    ret(ctx, 0);
}

void IoLseek32(Runtime &rt, Context &ctx) {
    ret(ctx, static_cast<std::uint32_t>(do_seek(rt, sarg(ctx, 0), sarg(ctx, 1), sarg(ctx, 2))));
}

void IoLseek32Async(Runtime &rt, Context &ctx) {
    OpenFile *f = file(rt, sarg(ctx, 0));
    if (f == nullptr) {
        ret(ctx, error::kIoBadFile);
        return;
    }
    f->async_result = do_seek(rt, sarg(ctx, 0), sarg(ctx, 1), sarg(ctx, 2));
    f->async_pending = true;
    ret(ctx, 0);
}

void finish_async(Runtime &rt, Context &ctx, bool poll) {
    const int fd = sarg(ctx, 0);
    OpenFile *f = file(rt, fd);
    if (f == nullptr) {
        ret(ctx, error::kIoBadFile);
        return;
    }
    if (!f->async_pending) {
        ret(ctx, poll ? 1u : 0x80010000u | 0x2Fu); // nothing pending
        return;
    }
    const std::uint32_t out = arg(ctx, 1);
    rt.memory().store32(out, static_cast<std::uint32_t>(f->async_result));
    rt.memory().store32(out + 4, static_cast<std::uint32_t>(static_cast<std::uint64_t>(f->async_result) >> 32u));
    f->async_pending = false;
    if (f->psp_path == "\x01close") {
        if (f->file != nullptr) std::fclose(f->file);
        state(rt).files.erase(fd);
    }
    ret(ctx, 0);
}

void IoWaitAsync(Runtime &rt, Context &ctx) { finish_async(rt, ctx, false); }
void IoPollAsync(Runtime &rt, Context &ctx) { finish_async(rt, ctx, true); }
void IoGetAsyncStat(Runtime &rt, Context &ctx) { finish_async(rt, ctx, arg(ctx, 1) != 0); }

std::vector<std::uint8_t> make_disc_stat(const IsoImage::Entry &e) {
    std::vector<std::uint8_t> out(0x58, 0);
    const std::uint32_t mode = e.is_directory ? 0x116Du : 0x216Du; // read/execute only
    const std::uint32_t attr = e.is_directory ? 0x10u : 0x20u;
    const std::uint64_t size = e.is_directory ? 0u : e.size;
    std::memcpy(out.data() + 0, &mode, 4);
    std::memcpy(out.data() + 4, &attr, 4);
    std::memcpy(out.data() + 8, &size, 8);
    std::memcpy(out.data() + 0x40, &e.lba, 4); // st_private[0]: start sector, as the PSP reports it
    return out;
}

std::optional<IsoImage::Entry> disc_entry(IsoImage &disc, const std::string &inner) {
    if (inner == "/") return IsoImage::Entry{"/", 0, 0, true};
    return disc.find(inner);
}

void IoGetstat(Runtime &rt, Context &ctx) {
    {
        auto &s = state(rt);
        const auto [device, inner] = split_device(rt, read_string(rt, arg(ctx, 0)));
        if (s.disc && is_disc_device(device)) {
            const auto e = disc_entry(*s.disc, inner);
            if (!e) {
                ret(ctx, error::kIoFileNotFound);
                return;
            }
            rt.memory().copy_in(arg(ctx, 1), make_disc_stat(*e));
            ret(ctx, 0);
            return;
        }
    }
    const auto host = host_path(rt, read_string(rt, arg(ctx, 0)));
    std::error_code ec;
    if (!host || !fs::exists(*host, ec)) {
        ret(ctx, error::kIoFileNotFound);
        return;
    }
    const auto st = make_stat(*host);
    rt.memory().copy_in(arg(ctx, 1), st);
    ret(ctx, 0);
}

void IoDopen(Runtime &rt, Context &ctx) {
    {
        auto &s = state(rt);
        const auto [device, inner] = split_device(rt, read_string(rt, arg(ctx, 0)));
        if (s.disc && is_disc_device(device)) {
            const auto e = disc_entry(*s.disc, inner);
            if (!e || !e->is_directory) {
                ret(ctx, error::kIoFileNotFound);
                return;
            }
            DiscDir d;
            d.entries.push_back(IsoImage::Entry{".", 0, 0, true});
            d.entries.push_back(IsoImage::Entry{"..", 0, 0, true});
            const std::string prefix = inner == "/" ? "/" : inner + "/";
            for (const auto &child : s.disc->entries()) {
                if (child.path.size() > prefix.size() && child.path.compare(0, prefix.size(), prefix) == 0 &&
                    child.path.find('/', prefix.size()) == std::string::npos) {
                    d.entries.push_back(child);
                }
            }
            const int fd = allocate_fd(s);
            if (fd < 0) {
                ret(ctx, error::kIoTooManyOpen);
                return;
            }
            s.disc_dirs[fd] = std::move(d);
            ret(ctx, static_cast<std::uint32_t>(fd));
            return;
        }
    }
    const auto host = host_path(rt, read_string(rt, arg(ctx, 0)));
    std::error_code ec;
    if (!host || !fs::is_directory(*host, ec)) {
        ret(ctx, error::kIoFileNotFound);
        return;
    }
    auto &s = state(rt);
    const int fd = allocate_fd(s);
    if (fd < 0) {
        ret(ctx, error::kIoTooManyOpen);
        return;
    }
    OpenDir d;
    // The PSP lists "." and ".." first.
    d.entries.emplace_back(*host / ".");
    d.entries.emplace_back(*host / "..");
    for (const auto &e : fs::directory_iterator(*host, ec)) d.entries.push_back(e);
    s.dirs[fd] = std::move(d);
    ret(ctx, static_cast<std::uint32_t>(fd));
}

void IoDread(Runtime &rt, Context &ctx) {
    auto &s = state(rt);
    if (const auto dd = s.disc_dirs.find(sarg(ctx, 0)); dd != s.disc_dirs.end()) {
        auto &d = dd->second;
        if (d.next >= d.entries.size()) {
            ret(ctx, 0);
            return;
        }
        const auto &e = d.entries[d.next++];
        const std::uint32_t dirent = arg(ctx, 1);
        rt.memory().copy_in(dirent, make_disc_stat(e));
        const std::string name = e.path.substr(e.path.rfind('/') + 1);
        std::vector<std::uint8_t> name_bytes(256, 0);
        std::memcpy(name_bytes.data(), name.c_str(), std::min<std::size_t>(name.size(), 255));
        rt.memory().copy_in(dirent + 0x58, name_bytes);
        ret(ctx, 1);
        return;
    }
    const auto it = s.dirs.find(sarg(ctx, 0));
    if (it == s.dirs.end()) {
        ret(ctx, error::kIoBadFile);
        return;
    }
    auto &d = it->second;
    if (d.next >= d.entries.size()) {
        ret(ctx, 0);
        return;
    }
    const auto &entry = d.entries[d.next++];
    const std::uint32_t dirent = arg(ctx, 1);
    const std::string name = entry.path().filename().string();
    auto st = make_stat(entry.path().filename() == "." || entry.path().filename() == ".." ? entry.path().parent_path()
                                                                                          : entry.path());
    rt.memory().copy_in(dirent, st);
    std::vector<std::uint8_t> name_bytes(256, 0);
    std::memcpy(name_bytes.data(), name.c_str(), std::min<std::size_t>(name.size(), 255));
    rt.memory().copy_in(dirent + 0x58, name_bytes);
    ret(ctx, 1);
}

void IoDclose(Runtime &rt, Context &ctx) {
    auto &s = state(rt);
    const bool closed = s.dirs.erase(sarg(ctx, 0)) != 0 || s.disc_dirs.erase(sarg(ctx, 0)) != 0;
    ret(ctx, closed ? 0u : error::kIoBadFile);
}

void IoChdir(Runtime &rt, Context &ctx) {
    std::string path = read_string(rt, arg(ctx, 0));
    if (path.find(':') == std::string::npos) path = state(rt).cwd + "/" + path;
    state(rt).cwd = path;
    ret(ctx, 0);
}

void IoMkdir(Runtime &rt, Context &ctx) {
    const auto host = host_path(rt, read_string(rt, arg(ctx, 0)));
    std::error_code ec;
    if (!host) {
        ret(ctx, error::kIoNoDevice);
        return;
    }
    if (fs::exists(*host, ec)) {
        ret(ctx, 0x80010011u);
        return;
    }
    ret(ctx, fs::create_directories(*host, ec) ? 0u : error::kIoFileNotFound);
}

void IoRmdir(Runtime &rt, Context &ctx) {
    const auto host = host_path(rt, read_string(rt, arg(ctx, 0)));
    std::error_code ec;
    ret(ctx, host && fs::is_directory(*host, ec) && fs::remove(*host, ec) ? 0u : error::kIoFileNotFound);
}

void IoRemove(Runtime &rt, Context &ctx) {
    const auto host = host_path(rt, read_string(rt, arg(ctx, 0)));
    std::error_code ec;
    ret(ctx, host && fs::is_regular_file(*host, ec) && fs::remove(*host, ec) ? 0u : error::kIoFileNotFound);
}

void IoRename(Runtime &rt, Context &ctx) {
    const auto from = host_path(rt, read_string(rt, arg(ctx, 0)));
    const auto to = host_path(rt, read_string(rt, arg(ctx, 1)));
    std::error_code ec;
    if (!from || !to || !fs::exists(*from, ec)) {
        ret(ctx, error::kIoFileNotFound);
        return;
    }
    fs::rename(*from, *to, ec);
    ret(ctx, ec ? error::kIoFileNotFound : 0u);
}

void IoDevctl(Runtime &rt, Context &ctx) {
    const std::uint32_t cmd = arg(ctx, 1);
    const std::uint32_t out = arg(ctx, 4);
    const std::uint32_t out_len = arg(ctx, 5);
    if (cmd == 0x02425818u && arg(ctx, 2) != 0) {
        // Memory stick free space: {max clusters, free clusters, max sectors?, sector size, sectors per cluster}.
        const std::uint32_t info = rt.memory().load32(arg(ctx, 2));
        const std::uint32_t values[] = {0x10000u, 0x8000u, 0x8000u, 512u, 64u};
        for (std::uint32_t i = 0; i < 5; ++i) rt.memory().store32(info + i * 4, values[i]);
    } else if (cmd == 0x02415821u && arg(ctx, 2) != 0) {
        // Register a memory stick insert/eject callback: the stick is always in, which the system reports at once
        // by notifying the new callback with "inserted" (1).
        const auto uid = static_cast<int>(rt.memory().load32(arg(ctx, 2)));
        if (auto it = rt.kernel().callbacks.find(uid); it != rt.kernel().callbacks.end()) {
            rt.kernel().notify_callback(it->second, 1);
        }
        std::fprintf(stderr, "[hle] memory stick callback %d registered (%s)\n", uid,
                     rt.kernel().callbacks.count(uid) ? "notified: inserted" : "unknown uid");
    } else if ((cmd == 0x02025806u || cmd == 0x02025801u) && out != 0 && out_len >= 4) {
        rt.memory().store32(out, cmd == 0x02025806u ? 1u : 4u); // stick inserted / FAT ready
    }
    ret(ctx, 0);
}

void IoIoctl(Runtime &, Context &ctx) { ret(ctx, 0); }
void IoChangeAsyncPriority(Runtime &, Context &ctx) { ret(ctx, 0); }
void IoSetAsyncCallback(Runtime &, Context &ctx) { ret(ctx, 0); }
void IoAssign(Runtime &, Context &ctx) { ret(ctx, 0); }
void IoGetDevType(Runtime &, Context &ctx) { ret(ctx, 0x20u); } // block device

void Stdin(Runtime &, Context &ctx) { ret(ctx, 0); }
void Stdout(Runtime &, Context &ctx) { ret(ctx, 1); }
void Stderr(Runtime &, Context &ctx) { ret(ctx, 2); }

const HleFunction kFunctions[] = {
    {"IoFileMgrForUser", 0x109F50BCu, "sceIoOpen", &IoOpen},
    {"IoFileMgrForUser", 0x89AA9906u, "sceIoOpenAsync", &IoOpenAsync},
    {"IoFileMgrForUser", 0x810C4BC3u, "sceIoClose", &IoClose},
    {"IoFileMgrForUser", 0xFF5940B6u, "sceIoCloseAsync", &IoCloseAsync},
    {"IoFileMgrForUser", 0x6A638D83u, "sceIoRead", &IoRead},
    {"IoFileMgrForUser", 0xA0B5A7C2u, "sceIoReadAsync", &IoReadAsync},
    {"IoFileMgrForUser", 0x42EC03ACu, "sceIoWrite", &IoWrite},
    {"IoFileMgrForUser", 0x0FACAB19u, "sceIoWriteAsync", &IoWriteAsync},
    {"IoFileMgrForUser", 0x27EB27B8u, "sceIoLseek", &IoLseek},
    {"IoFileMgrForUser", 0x71B19E77u, "sceIoLseekAsync", &IoLseekAsync},
    {"IoFileMgrForUser", 0x68963324u, "sceIoLseek32", &IoLseek32},
    {"IoFileMgrForUser", 0x1B385D8Fu, "sceIoLseek32Async", &IoLseek32Async},
    {"IoFileMgrForUser", 0xE23EEC33u, "sceIoWaitAsync", &IoWaitAsync},
    {"IoFileMgrForUser", 0x35DBD746u, "sceIoWaitAsyncCB", &IoWaitAsync},
    {"IoFileMgrForUser", 0x3251EA56u, "sceIoPollAsync", &IoPollAsync},
    {"IoFileMgrForUser", 0xCB05F8D6u, "sceIoGetAsyncStat", &IoGetAsyncStat},
    {"IoFileMgrForUser", 0xACE946E8u, "sceIoGetstat", &IoGetstat},
    {"IoFileMgrForUser", 0xB29DDF9Cu, "sceIoDopen", &IoDopen},
    {"IoFileMgrForUser", 0xE3EB004Cu, "sceIoDread", &IoDread},
    {"IoFileMgrForUser", 0xEB092469u, "sceIoDclose", &IoDclose},
    {"IoFileMgrForUser", 0x55F4717Du, "sceIoChdir", &IoChdir},
    {"IoFileMgrForUser", 0x06A70004u, "sceIoMkdir", &IoMkdir},
    {"IoFileMgrForUser", 0x1117C65Fu, "sceIoRmdir", &IoRmdir},
    {"IoFileMgrForUser", 0xF27A9C51u, "sceIoRemove", &IoRemove},
    {"IoFileMgrForUser", 0x779103A0u, "sceIoRename", &IoRename},
    {"IoFileMgrForUser", 0x54F5FB11u, "sceIoDevctl", &IoDevctl},
    {"IoFileMgrForUser", 0x63632449u, "sceIoIoctl", &IoIoctl},
    {"IoFileMgrForUser", 0xB293727Fu, "sceIoChangeAsyncPriority", &IoChangeAsyncPriority},
    {"IoFileMgrForUser", 0xA12A0514u, "sceIoSetAsyncCallback", &IoSetAsyncCallback},
    {"IoFileMgrForUser", 0xB2A628C1u, "sceIoAssign", &IoAssign},
    {"IoFileMgrForUser", 0x08BD7374u, "sceIoGetDevType", &IoGetDevType},
    {"StdioForUser", 0x172D316Eu, "sceKernelStdin", &Stdin},
    {"StdioForUser", 0xA6BAB2E9u, "sceKernelStdout", &Stdout},
    {"StdioForUser", 0xF78BA90Au, "sceKernelStderr", &Stderr},
};

} // namespace

std::optional<fs::path> resolve_under_root(const fs::path &root, const std::string &rest) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= rest.size()) {
        const auto slash = rest.find_first_of("/\\", start);
        const std::string part = rest.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..") {
            if (parts.empty()) return std::nullopt;
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            // `path /= "C:"` would replace the whole path on Windows, and "a:b" names an NTFS stream.
            if (part.find_first_of(":*?\"<>|") != std::string::npos) return std::nullopt;
            parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    fs::path result = root;
    for (const auto &p : parts) result /= p;
    return result;
}

std::span<const HleFunction> io_functions() { return kFunctions; }

void set_working_directory(Runtime &rt, const std::string &psp_path) { state(rt).cwd = psp_path; }

} // namespace mhf::psp::hle
