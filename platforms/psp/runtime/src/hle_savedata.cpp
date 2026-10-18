// sceUtility savedata: the work behind sceUtilitySavedataInitStart, without a dialog.
//
// Saves live in plain folders on the host, "<memstick>/PSP/SAVEDATA/<gameName><saveName>/", with the game's data file
// stored as the game handed it over (the real system encrypts it; we do not, and never decrypt anything), a
// PARAM.SFO with the titles shown in lists, and the icon/picture/sound files the game provides.
//
// Where the real system would show a list for the player to pick from, a slot is chosen from the game's list and its
// focus hint (most recent save for loads, first empty slot for new saves) and written back to saveName, so the game
// sees the same result as if the player had picked it. A save-slot UI in the frontend can replace that choice later.
//
// Parameter layout: pspsdk psputility_savedata.h (SceUtilitySavedataParam, firmware >= 2.00).

#include "hle.hpp"
#include "param_sfo.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace mhf::psp::hle {

namespace {

namespace fs = std::filesystem;

// Offsets in SceUtilitySavedataParam (checked with offsetof() against the pspsdk header; size 0x600).
constexpr std::uint32_t kResult = 0x1C;
constexpr std::uint32_t kMode = 0x30;
constexpr std::uint32_t kGameName = 0x3C;   // char[13]
constexpr std::uint32_t kSaveName = 0x4C;   // char[20]
constexpr std::uint32_t kSaveNameList = 0x60;
constexpr std::uint32_t kFileName = 0x64;   // char[13]
constexpr std::uint32_t kDataBuf = 0x74;
constexpr std::uint32_t kDataBufSize = 0x78;
constexpr std::uint32_t kDataSize = 0x7C;
constexpr std::uint32_t kSfoTitle = 0x80;          // char[0x80]
constexpr std::uint32_t kSfoSavedataTitle = 0x100; // char[0x80]
constexpr std::uint32_t kSfoDetail = 0x180;        // char[0x400]
constexpr std::uint32_t kSfoParental = 0x580;      // u8
constexpr std::uint32_t kIcon0 = 0x584;            // {buf, bufSize, size, unknown} each
constexpr std::uint32_t kIcon1 = 0x594;
constexpr std::uint32_t kPic1 = 0x5A4;
constexpr std::uint32_t kSnd0 = 0x5B4;
constexpr std::uint32_t kFocus = 0x5C8;
constexpr std::uint32_t kMsFree = 0x5D0;
constexpr std::uint32_t kMsData = 0x5D4;
constexpr std::uint32_t kUtilityData = 0x5D8;
constexpr std::uint32_t kIdList = 0x5F4;
constexpr std::uint32_t kFileList = 0x5F8;
constexpr std::uint32_t kSizeInfo = 0x5FC;

enum Mode : std::uint32_t {
    kAutoLoad = 0, kAutoSave = 1, kLoad = 2, kSave = 3, kListLoad = 4, kListSave = 5, kListDelete = 6,
    kListAllDelete = 7, kSizes = 8, kAutoDelete = 9, kDelete = 10, kList = 11, kFiles = 12, kMakeDataSecure = 13,
    kMakeData = 14, kReadDataSecure = 15, kReadData = 16, kWriteDataSecure = 17, kWriteData = 18, kEraseSecure = 19,
    kErase = 20, kDeleteData = 21, kGetSize = 22,
};

enum Focus : std::uint32_t {
    kFocusFirstList = 1, kFocusLastList = 2, kFocusLatest = 3, kFocusOldest = 4, kFocusFirstData = 5,
    kFocusLastData = 6, kFocusFirstEmpty = 7, kFocusLastEmpty = 8,
};

constexpr std::uint32_t kLoadNoData = 0x80110307u;
constexpr std::uint32_t kLoadNoFile = 0x80110309u;
constexpr std::uint32_t kSaveParam = 0x80110388u;
constexpr std::uint32_t kSaveAccess = 0x80110385u;
constexpr std::uint32_t kDeleteNoData = 0x80110347u;
constexpr std::uint32_t kSizesNoData = 0x801103C7u;
constexpr std::uint32_t kMcNoData = 0x80110327u;
constexpr std::uint32_t kMcNoFile = 0x80110329u;
constexpr std::uint32_t kMcAccess = 0x80110325u;
constexpr std::uint32_t kTypeError = 0x80110300u;

constexpr std::uint32_t kClusterSize = 0x8000u;          // 32 KiB, as the real memory stick reports
constexpr std::uint32_t kFreeClusters = 0x8000u;         // report 1 GiB free
constexpr std::uint32_t kFreeKB = kClusterSize / 1024u * kFreeClusters;

// Names come from the game; keep them to a single path component.
std::string sanitize(std::string s) {
    for (char &c : s) {
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) c = '_';
    }
    if (s == "." || s == "..") s = "_";
    return s;
}

std::uint64_t clusters_for(std::uint64_t bytes) { return (bytes + kClusterSize - 1u) / kClusterSize; }

void store_string(Runtime &rt, std::uint32_t address, const std::string &s, std::size_t capacity) {
    auto &mem = rt.memory();
    mem.zero(address, capacity);
    const std::size_t n = std::min(s.size(), capacity - 1u);
    for (std::size_t i = 0; i < n; ++i) mem.store8(address + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(s[i]));
}

std::vector<std::uint8_t> read_guest(Runtime &rt, std::uint32_t address, std::uint32_t size) {
    std::vector<std::uint8_t> out(size);
    if (size != 0) rt.memory().copy_out(address, out);
    return out;
}

std::optional<std::vector<std::uint8_t>> read_host(const fs::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
}

bool write_host(const fs::path &path, const std::vector<std::uint8_t> &data) {
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec); // replace atomically so a crash never leaves half a save
    return !ec;
}

// Host file time → microseconds since the Unix epoch.
std::uint64_t file_time_us(const fs::path &path) {
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    if (ec) return 0;
    // Shift by the offset between the two clocks now (std::chrono::clock_cast is missing from some standard libraries).
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(sys.time_since_epoch()).count());
}

// ScePspDateTime: u16 year, month, day, hour, minute, second; u32 microsecond (local time).
void store_datetime(Runtime &rt, std::uint32_t address, std::uint64_t unix_us) {
    const auto t = static_cast<std::time_t>(unix_us / 1'000'000u);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    auto &mem = rt.memory();
    mem.store16(address + 0, static_cast<std::uint16_t>(tm.tm_year + 1900));
    mem.store16(address + 2, static_cast<std::uint16_t>(tm.tm_mon + 1));
    mem.store16(address + 4, static_cast<std::uint16_t>(tm.tm_mday));
    mem.store16(address + 6, static_cast<std::uint16_t>(tm.tm_hour));
    mem.store16(address + 8, static_cast<std::uint16_t>(tm.tm_min));
    mem.store16(address + 10, static_cast<std::uint16_t>(tm.tm_sec));
    mem.store32(address + 12, static_cast<std::uint32_t>(unix_us % 1'000'000u));
}

// ---------------------------------------------------------------- PARAM.SFO

struct SfoEntry {
    std::string key;
    bool is_int{};
    std::string text;
    std::uint32_t value{};
    std::uint32_t max_length{};
};

void put16(std::vector<std::uint8_t> &v, std::size_t at, std::uint32_t x) {
    v[at] = static_cast<std::uint8_t>(x);
    v[at + 1] = static_cast<std::uint8_t>(x >> 8u);
}
void put32(std::vector<std::uint8_t> &v, std::size_t at, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x >> (8 * i));
}
std::uint32_t get16(const std::vector<std::uint8_t> &v, std::size_t at) { return v[at] | (v[at + 1] << 8u); }
std::uint32_t get32(const std::vector<std::uint8_t> &v, std::size_t at) {
    return v[at] | (v[at + 1] << 8u) | (v[at + 2] << 16u) | (static_cast<std::uint32_t>(v[at + 3]) << 24u);
}

std::vector<std::uint8_t> build_sfo(std::vector<SfoEntry> entries) {
    std::sort(entries.begin(), entries.end(), [](const SfoEntry &a, const SfoEntry &b) { return a.key < b.key; });
    const std::size_t count = entries.size();
    const std::size_t key_table = 20u + 16u * count;
    std::size_t keys_size = 0;
    for (const auto &e : entries) keys_size += e.key.size() + 1u;
    const std::size_t data_table = (key_table + keys_size + 3u) & ~std::size_t{3};
    std::size_t data_size = 0;
    for (auto &e : entries) {
        if (e.is_int) e.max_length = 4;
        else e.max_length = std::max<std::uint32_t>(e.max_length, static_cast<std::uint32_t>(e.text.size() + 1u));
        e.max_length = (e.max_length + 3u) & ~3u;
        data_size += e.max_length;
    }
    std::vector<std::uint8_t> out(data_table + data_size, 0);
    out[0] = 0; out[1] = 'P'; out[2] = 'S'; out[3] = 'F';
    put32(out, 4, 0x0101u);
    put32(out, 8, static_cast<std::uint32_t>(key_table));
    put32(out, 12, static_cast<std::uint32_t>(data_table));
    put32(out, 16, static_cast<std::uint32_t>(count));
    std::size_t key_at = 0, data_at = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto &e = entries[i];
        const std::size_t h = 20u + 16u * i;
        put16(out, h, static_cast<std::uint32_t>(key_at));
        put16(out, h + 2, e.is_int ? 0x0404u : 0x0204u);
        put32(out, h + 4, e.is_int ? 4u : static_cast<std::uint32_t>(e.text.size() + 1u));
        put32(out, h + 8, e.max_length);
        put32(out, h + 12, static_cast<std::uint32_t>(data_at));
        std::copy(e.key.begin(), e.key.end(), out.begin() + static_cast<std::ptrdiff_t>(key_table + key_at));
        key_at += e.key.size() + 1u;
        if (e.is_int) put32(out, data_table + data_at, e.value);
        else std::copy(e.text.begin(), e.text.end(), out.begin() + static_cast<std::ptrdiff_t>(data_table + data_at));
        data_at += e.max_length;
    }
    return out;
}

std::vector<SfoEntry> parse_sfo(const std::vector<std::uint8_t> &v) {
    std::vector<SfoEntry> out;
    if (v.size() < 20 || v[1] != 'P' || v[2] != 'S' || v[3] != 'F') return out;
    const std::size_t key_table = get32(v, 8), data_table = get32(v, 12), count = get32(v, 16);
    for (std::size_t i = 0; i < count && 20u + 16u * (i + 1) <= v.size(); ++i) {
        const std::size_t h = 20u + 16u * i;
        SfoEntry e;
        std::size_t k = key_table + get16(v, h);
        while (k < v.size() && v[k] != 0) e.key.push_back(static_cast<char>(v[k++]));
        const std::uint32_t fmt = get16(v, h + 2), length = get32(v, h + 4);
        const std::size_t at = data_table + get32(v, h + 12);
        if (at + length > v.size()) continue;
        if (fmt == 0x0404u) {
            e.is_int = true;
            e.value = get32(v, at);
        } else {
            for (std::size_t j = 0; j < length && v[at + j] != 0; ++j) e.text.push_back(static_cast<char>(v[at + j]));
        }
        out.push_back(std::move(e));
    }
    return out;
}

// ---------------------------------------------------------------- one request

struct Request {
    Runtime &rt;
    std::uint32_t p;
    psprecomp::GuestMemory &mem;
    std::optional<std::string> chosen{}; // slot the player picked in our list dialog (list modes)

    std::uint32_t u32(std::uint32_t off) const { return mem.load32(p + off); }
    std::string str(std::uint32_t off, std::size_t n) const { return read_string(rt, p + off, n); }

    std::string game_name() const { return sanitize(str(kGameName, 13)); }
    // "<>" stands for "no save name": the folder is the game name alone.
    std::string save_name() const {
        std::string s = str(kSaveName, 20);
        return s == "<>" ? std::string() : sanitize(std::move(s));
    }
    std::string file_name() const { return sanitize(str(kFileName, 13)); }

    fs::path root() const { return rt.options().memstick_root / "PSP" / "SAVEDATA"; }
    fs::path dir_for(const std::string &save) const { return root() / (game_name() + save); }

    static bool exists(const fs::path &dir) {
        std::error_code ec;
        return fs::is_directory(dir, ec);
    }

    // The game's slot list (char[20] entries, ended by an empty one).
    std::vector<std::string> name_list() const {
        std::vector<std::string> out;
        const std::uint32_t list = u32(kSaveNameList);
        if (list == 0) return out;
        for (std::uint32_t i = 0; i < 1024; ++i) {
            std::string s = read_string(rt, list + i * 20u, 20);
            if (s.empty()) break;
            out.push_back(sanitize(std::move(s)));
        }
        return out;
    }

    // The slot the player would have picked in the list dialog; nullopt when there is nothing to pick.
    std::optional<std::string> pick(bool for_save) const {
        if (chosen) return chosen;
        std::vector<std::string> list = name_list();
        if (list.empty()) {
            return save_name();
        }
        const std::uint32_t focus = u32(kFocus);
        std::vector<std::string> existing, empty;
        for (const auto &s : list) (exists(dir_for(s)) ? existing : empty).push_back(s);
        const auto newest = [&](bool oldest) -> std::optional<std::string> {
            if (existing.empty()) return std::nullopt;
            auto best = existing.front();
            for (const auto &s : existing) {
                const auto a = file_time_us(dir_for(s) / "PARAM.SFO"), b = file_time_us(dir_for(best) / "PARAM.SFO");
                if (oldest ? a < b : a > b) best = s;
            }
            return best;
        };
        if (!for_save) {
            if (focus == kFocusOldest) return newest(true);
            if (focus == kFocusLastList || focus == kFocusLastData)
                return existing.empty() ? std::nullopt : std::optional<std::string>(existing.back());
            if (focus == kFocusFirstList || focus == kFocusFirstData)
                return existing.empty() ? std::nullopt : std::optional<std::string>(existing.front());
            return newest(false);
        }
        switch (focus) {
        case kFocusLastList: return list.back();
        case kFocusLatest: if (auto s = newest(false)) return s; break;
        case kFocusOldest: if (auto s = newest(true)) return s; break;
        case kFocusFirstData: if (!existing.empty()) return existing.front(); break;
        case kFocusLastData: if (!existing.empty()) return existing.back(); break;
        case kFocusLastEmpty: if (!empty.empty()) return empty.back(); break;
        case kFocusFirstList: return list.front();
        default: if (!empty.empty()) return empty.front(); break;
        }
        return list.front();
    }

    void set_save_name(const std::string &s) const { store_string(rt, p + kSaveName, s, 20); }

    std::uint64_t pending_bytes() const {
        std::uint64_t total = u32(kDataSize);
        for (const std::uint32_t f : {kIcon0, kIcon1, kPic1, kSnd0}) total += mem.load32(p + f + 8u);
        return total + 0x1000u; // PARAM.SFO
    }

    // ---- operations

    std::uint32_t load(const fs::path &dir, bool multi_call) const {
        const std::uint32_t no_data = multi_call ? kMcNoData : kLoadNoData;
        const std::uint32_t no_file = multi_call ? kMcNoFile : kLoadNoFile;
        if (!exists(dir)) return no_data;
        if (const auto sfo = read_host(dir / "PARAM.SFO")) {
            for (const auto &e : parse_sfo(*sfo)) {
                if (e.key == "TITLE") store_string(rt, p + kSfoTitle, e.text, 0x80);
                else if (e.key == "SAVEDATA_TITLE") store_string(rt, p + kSfoSavedataTitle, e.text, 0x80);
                else if (e.key == "SAVEDATA_DETAIL") store_string(rt, p + kSfoDetail, e.text, 0x400);
                else if (e.key == "PARENTAL_LEVEL") mem.store8(p + kSfoParental, static_cast<std::uint8_t>(e.value));
            }
        }
        const std::string file = file_name();
        if (file.empty()) return 0;
        const auto data = read_host(dir / file);
        if (!data) return no_file;
        const std::uint32_t buffer = u32(kDataBuf), capacity = u32(kDataBufSize);
        const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(data->size(), capacity));
        if (buffer != 0 && n != 0) mem.copy_in(buffer, std::span(data->data(), n));
        mem.store32(p + kDataSize, n);
        return 0;
    }

    std::uint32_t save(const fs::path &dir, bool multi_call, bool write_sfo) const {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) return multi_call ? kMcAccess : kSaveAccess;
        if (const std::string file = file_name(); !file.empty()) {
            const std::uint32_t buffer = u32(kDataBuf), size = u32(kDataSize);
            if (buffer == 0 && size != 0) return multi_call ? kMcAccess : kSaveParam;
            if (!write_host(dir / file, read_guest(rt, buffer, size))) return multi_call ? kMcAccess : kSaveAccess;
        }
        const struct { std::uint32_t field; const char *name; } extras[] = {
            {kIcon0, "ICON0.PNG"}, {kIcon1, "ICON1.PMF"}, {kPic1, "PIC1.PNG"}, {kSnd0, "SND0.AT3"}};
        for (const auto &x : extras) {
            const std::uint32_t buffer = mem.load32(p + x.field), size = mem.load32(p + x.field + 8u);
            if (buffer != 0 && size != 0) write_host(dir / x.name, read_guest(rt, buffer, size));
        }
        if (write_sfo) {
            const std::string directory = dir.filename().string();
            std::vector<SfoEntry> sfo{
                {"CATEGORY", false, "MS", 0, 4},
                {"PARENTAL_LEVEL", true, {}, mem.load8(p + kSfoParental), 4},
                {"SAVEDATA_DETAIL", false, str(kSfoDetail, 0x400), 0, 0x400},
                {"SAVEDATA_DIRECTORY", false, directory, 0, 0x40},
                {"SAVEDATA_TITLE", false, str(kSfoSavedataTitle, 0x80), 0, 0x80},
                {"TITLE", false, str(kSfoTitle, 0x80), 0, 0x80},
            };
            if (!write_host(dir / "PARAM.SFO", build_sfo(std::move(sfo)))) return multi_call ? kMcAccess : kSaveAccess;
        }
        return 0;
    }

    static std::uint64_t folder_bytes(const fs::path &dir, std::uint64_t *clusters = nullptr) {
        std::uint64_t bytes = 0, used = 0;
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            const auto size = e.file_size(ec);
            bytes += size;
            used += clusters_for(size);
        }
        if (clusters != nullptr) *clusters = used + 1u; // the folder itself
        return bytes;
    }

    void store_size_text(std::uint32_t address, std::uint64_t kb) const {
        char text[16];
        std::snprintf(text, sizeof text, "%llu KB", static_cast<unsigned long long>(kb));
        store_string(rt, address, text, 8);
    }

    std::uint32_t sizes() const {
        std::uint32_t result = 0;
        if (const std::uint32_t free = u32(kMsFree); free != 0) {
            mem.store32(free + 0, kClusterSize);
            mem.store32(free + 4, kFreeClusters);
            mem.store32(free + 8, kFreeKB);
            store_size_text(free + 12, kFreeKB);
        }
        if (const std::uint32_t data = u32(kMsData); data != 0) {
            const std::string game = sanitize(read_string(rt, data, 13));
            std::string save = read_string(rt, data + 16u, 20);
            save = save == "<>" ? std::string() : sanitize(std::move(save));
            const fs::path dir = root() / (game + save);
            const std::uint32_t info = data + 36u;
            mem.zero(info, 28); // SceUtilitySavedataUsedDataInfo
            if (exists(dir)) {
                std::uint64_t clusters = 0;
                folder_bytes(dir, &clusters);
                const auto kb = clusters * (kClusterSize / 1024u);
                mem.store32(info + 0, static_cast<std::uint32_t>(clusters));
                mem.store32(info + 4, static_cast<std::uint32_t>(kb));
                store_size_text(info + 8, kb);
                mem.store32(info + 16, static_cast<std::uint32_t>(kb));
                store_size_text(info + 20, kb);
            } else {
                result = kSizesNoData;
            }
        }
        if (const std::uint32_t utility = u32(kUtilityData); utility != 0) {
            const std::uint64_t clusters = clusters_for(pending_bytes()) + 1u;
            const auto kb = clusters * (kClusterSize / 1024u);
            mem.store32(utility + 0, static_cast<std::uint32_t>(clusters));
            mem.store32(utility + 4, static_cast<std::uint32_t>(kb));
            store_size_text(utility + 8, kb);
            mem.store32(utility + 16, static_cast<std::uint32_t>(kb));
            store_size_text(utility + 20, kb);
        }
        return result;
    }

    // LIST: the game's saves whose folder name matches gameName + saveName ("<>" = anything).
    std::uint32_t list() const {
        const std::uint32_t info = u32(kIdList);
        if (info == 0) return kMcAccess;
        const std::uint32_t max = mem.load32(info + 0), entries = mem.load32(info + 8);
        const std::string prefix = game_name();
        std::string pattern = str(kSaveName, 20);
        const auto wildcard = pattern.find("<>");
        if (wildcard != std::string::npos) pattern.erase(wildcard);
        std::vector<fs::path> found;
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(root(), ec)) {
            const std::string name = e.path().filename().string();
            if (!e.is_directory() || name.rfind(prefix, 0) != 0) continue;
            const std::string rest = name.substr(prefix.size());
            if (wildcard == std::string::npos ? rest == pattern : rest.rfind(pattern, 0) == 0) found.push_back(e.path());
        }
        std::sort(found.begin(), found.end());
        std::uint32_t count = 0;
        for (const auto &dir : found) {
            if (count >= max || entries == 0) break;
            const std::uint32_t e = entries + count * 72u;
            const auto t = file_time_us(dir);
            mem.store32(e, 0x11FFu); // directory, rwx
            store_datetime(rt, e + 4, t);
            store_datetime(rt, e + 20, t);
            store_datetime(rt, e + 36, t);
            store_string(rt, e + 52, dir.filename().string().substr(prefix.size()), 20);
            ++count;
        }
        mem.store32(info + 4, count);
        return 0;
    }

    // FILES: the files in the save folder (all reported as normal files; PARAM.SFO as the system file).
    std::uint32_t files(const fs::path &dir) const {
        const std::uint32_t info = u32(kFileList);
        if (info == 0) return kMcAccess;
        if (!exists(dir)) return kMcNoData;
        const std::uint32_t max_normal = mem.load32(info + 4), max_system = mem.load32(info + 8);
        const std::uint32_t normal = mem.load32(info + 28), system = mem.load32(info + 32);
        std::uint32_t n_normal = 0, n_system = 0;
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            const std::string name = e.path().filename().string();
            if (name.size() > 15 || name.ends_with(".tmp")) continue;
            const bool is_system = name == "PARAM.SFO";
            std::uint32_t &n = is_system ? n_system : n_normal;
            const std::uint32_t base = is_system ? system : normal, max = is_system ? max_system : max_normal;
            if (base == 0 || n >= max) continue;
            const std::uint32_t entry = base + n * 80u;
            const auto t = file_time_us(e.path());
            const auto size = static_cast<std::uint64_t>(e.file_size(ec));
            mem.store32(entry + 0, 0x21FFu); // regular file, rwx
            mem.store32(entry + 4, 0);
            mem.store32(entry + 8, static_cast<std::uint32_t>(size));
            mem.store32(entry + 12, static_cast<std::uint32_t>(size >> 32u));
            store_datetime(rt, entry + 16, t);
            store_datetime(rt, entry + 32, t);
            store_datetime(rt, entry + 48, t);
            store_string(rt, entry + 64, name, 16);
            ++n;
        }
        mem.store32(info + 12, 0);
        mem.store32(info + 16, n_normal);
        mem.store32(info + 20, n_system);
        return 0;
    }

    std::uint32_t get_size(const fs::path &dir) const {
        const std::uint32_t info = u32(kSizeInfo);
        if (info == 0) return kMcAccess;
        std::uint64_t needed = 0;
        for (const std::uint32_t list : {0u, 1u}) {
            const std::uint32_t count = mem.load32(info + list * 4u), entries = mem.load32(info + 8u + list * 4u);
            for (std::uint32_t i = 0; entries != 0 && i < count; ++i) {
                const std::uint64_t size = mem.load32(entries + i * 24u) |
                                           (static_cast<std::uint64_t>(mem.load32(entries + i * 24u + 4u)) << 32u);
                needed += clusters_for(size);
            }
        }
        needed += 1u + clusters_for(0x1000u); // folder + PARAM.SFO
        std::uint64_t current = 0;
        if (exists(dir)) folder_bytes(dir, &current);
        const std::uint32_t kb_per_cluster = kClusterSize / 1024u;
        mem.store32(info + 16, kClusterSize);
        mem.store32(info + 20, kFreeClusters);
        mem.store32(info + 24, kFreeKB);
        store_size_text(info + 28, kFreeKB);
        mem.store32(info + 36, static_cast<std::uint32_t>(needed * kb_per_cluster));
        store_size_text(info + 40, needed * kb_per_cluster);
        const std::uint64_t overwrite = needed > current ? needed - current : 0;
        mem.store32(info + 48, static_cast<std::uint32_t>(overwrite * kb_per_cluster));
        store_size_text(info + 52, overwrite * kb_per_cluster);
        return 0;
    }

    static std::uint32_t remove_folder(const fs::path &dir, std::uint32_t no_data) {
        if (!exists(dir)) return no_data;
        std::error_code ec;
        fs::remove_all(dir, ec);
        return ec ? kMcAccess : 0u;
    }

    std::uint32_t run(std::uint32_t mode) const {
        switch (mode) {
        case kAutoLoad: case kLoad: return load(dir_for(save_name()), false);
        case kListLoad: {
            const auto s = pick(false);
            if (!s) return kLoadNoData;
            set_save_name(*s);
            return load(dir_for(*s), false);
        }
        case kAutoSave: case kSave: case kListSave: {
            const auto s = mode == kListSave ? pick(true) : std::optional<std::string>(save_name());
            if (!s) return kSaveParam;
            if (mode == kListSave) set_save_name(*s);
            return save(dir_for(*s), false, true);
        }
        case kListDelete: case kListAllDelete: {
            const auto s = pick(false);
            if (!s) return kDeleteNoData;
            set_save_name(*s);
            return remove_folder(dir_for(*s), kDeleteNoData);
        }
        case kAutoDelete: case kDelete: return remove_folder(dir_for(save_name()), kDeleteNoData);
        case kDeleteData: return remove_folder(dir_for(save_name()), kMcNoData);
        case kSizes: return sizes();
        case kList: return list();
        case kFiles: return files(dir_for(save_name()));
        case kMakeDataSecure: case kMakeData: return save(dir_for(save_name()), true, true);
        case kWriteDataSecure: case kWriteData: {
            const fs::path dir = dir_for(save_name());
            if (!exists(dir)) return kMcNoData;
            return save(dir, true, false);
        }
        case kReadDataSecure: case kReadData: return load(dir_for(save_name()), true);
        case kEraseSecure: case kErase: {
            const fs::path dir = dir_for(save_name());
            if (!exists(dir)) return kMcNoData;
            std::error_code ec;
            return fs::remove(dir / file_name(), ec) ? 0u : kMcNoFile;
        }
        case kGetSize: return get_size(dir_for(save_name()));
        default: return kTypeError;
        }
    }
};

const char *mode_name(std::uint32_t mode) {
    static const char *const names[] = {
        "autoload", "autosave", "load", "save", "listload", "listsave", "listdelete", "listalldelete",
        "sizes", "autodelete", "delete", "list", "files", "makedatasecure", "makedata", "readdatasecure",
        "readdata", "writedatasecure", "writedata", "erasesecure", "erase", "deletedata", "getsize"};
    return mode < std::size(names) ? names[mode] : "unknown";
}

} // namespace

SavedataPlan savedata_plan(Runtime &rt, std::uint32_t params) {
    const Request request{rt, params, rt.memory()};
    SavedataPlan plan;
    plan.mode = request.u32(kMode);
    switch (plan.mode) {
    case kAutoLoad: plan.ui = SavedataPlan::Ui::Toast; plan.action = SavedataPlan::Action::Load; break;
    case kAutoSave: plan.ui = SavedataPlan::Ui::Toast; plan.action = SavedataPlan::Action::Save; break;
    case kLoad: plan.ui = SavedataPlan::Ui::Confirm; plan.action = SavedataPlan::Action::Load; break;
    case kSave: plan.ui = SavedataPlan::Ui::Confirm; plan.action = SavedataPlan::Action::Save; break;
    case kDelete: plan.ui = SavedataPlan::Ui::Confirm; plan.action = SavedataPlan::Action::Delete; break;
    case kListLoad: plan.ui = SavedataPlan::Ui::List; plan.action = SavedataPlan::Action::Load; break;
    case kListSave: plan.ui = SavedataPlan::Ui::List; plan.action = SavedataPlan::Action::Save; break;
    case kListDelete: case kListAllDelete: plan.ui = SavedataPlan::Ui::List; plan.action = SavedataPlan::Action::Delete; break;
    default: plan.ui = SavedataPlan::Ui::None; break;
    }
    if (plan.ui == SavedataPlan::Ui::None) return plan;
    plan.new_title = request.str(kSfoSavedataTitle, 0x80);
    plan.new_detail = request.str(kSfoDetail, 0x400);
    std::vector<std::string> names = request.name_list();
    if (names.empty()) names.push_back(request.save_name());
    const auto describe = [&](const std::string &name) {
        SavedataSlot slot;
        slot.name = name;
        const fs::path dir = request.dir_for(name);
        slot.exists = Request::exists(dir);
        if (slot.exists) {
            std::ifstream in(dir / "PARAM.SFO", std::ios::binary);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (auto sfo = ParamSfo::parse(bytes)) {
                slot.title = sfo->string("SAVEDATA_TITLE");
                slot.detail = sfo->string("SAVEDATA_DETAIL");
                if (slot.title.empty()) slot.title = sfo->string("TITLE");
            }
            std::error_code ec;
            const auto when = fs::last_write_time(dir / "PARAM.SFO", ec);
            if (!ec) {
                const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(
                    when - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                const std::time_t t = std::chrono::system_clock::to_time_t(sys);
                char buf[32];
                std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", std::localtime(&t));
                slot.date = buf;
            }
        }
        return slot;
    };
    if (plan.ui == SavedataPlan::Ui::List) {
        for (const auto &n : names) {
            SavedataSlot slot = describe(n);
            if (!slot.exists && plan.action != SavedataPlan::Action::Save) continue;
            plan.slots.push_back(std::move(slot));
        }
        if (const auto focus = request.pick(plan.action == SavedataPlan::Action::Save)) {
            for (std::size_t i = 0; i < plan.slots.size(); ++i) {
                if (plan.slots[i].name == *focus) plan.focus = static_cast<int>(i);
            }
        }
    } else {
        plan.slots.push_back(describe(request.save_name()));
    }
    return plan;
}

std::uint32_t execute_savedata(Runtime &rt, std::uint32_t params, const std::optional<std::string> &chosen) {
    Request request{rt, params, rt.memory()};
    request.chosen = chosen;
    const std::uint32_t mode = request.u32(kMode);
    const std::uint32_t result = request.run(mode);
    if (chosen) request.set_save_name(*chosen);
    rt.memory().store32(params + kResult, result);
    std::string list;
    for (const auto &name : request.name_list()) list += (list.empty() ? "" : ",") + name;
    std::fprintf(stderr, "[hle] savedata %s %s%s file \"%s\" size %u/%u focus %u list [%s] -> %08X\n", mode_name(mode),
                 request.game_name().c_str(), request.save_name().c_str(), request.file_name().c_str(),
                 request.u32(kDataSize), request.u32(kDataBufSize), request.u32(kFocus), list.c_str(), result);
    return result;
}

} // namespace mhf::psp::hle
