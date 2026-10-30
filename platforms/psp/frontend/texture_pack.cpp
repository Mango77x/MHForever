// Texture packs in PPSSPP's format (texture_pack.hpp).

#include "texture_pack.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string_view>
#include <system_error>

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace mhf::psp::textures {
namespace fs = std::filesystem;

namespace {

// PPSSPP's seeds: the data hash and the palette hash.
constexpr std::uint32_t kDataSeed = 0xBACD7814u;
constexpr std::uint32_t kClutSeed = 0xC0108888u;

constexpr std::uint32_t kBitsPerTexel[11] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8};

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool parse_hex(std::string_view s, std::uint64_t &out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out, 16);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

template <typename T> bool parse_number(std::string_view s, T &out) {
    s = trim(s);
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        std::uint64_t v = 0;
        if (!parse_hex(s.substr(2), v)) return false;
        out = static_cast<T>(v);
        return true;
    }
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool parse_bool(std::string_view s) {
    const std::string v = lower(trim(s));
    return v == "true" || v == "1" || v == "yes";
}

// A key as packs write it: 16 hex digits of the cache part, then 8 of the data hash, then "_<level>". As PPSSPP's
// sscanf("%16llx%8x"), up to 16 digits go to the cache part and what follows to the data hash.
bool parse_key(std::string_view text, Key &key, int &level) {
    text = trim(text);
    level = 0;
    if (const auto underscore = text.find('_'); underscore != std::string_view::npos) {
        if (underscore != 24) return false;
        if (!parse_number(text.substr(underscore + 1), level) || level < 0) return false;
        text = text.substr(0, underscore);
    }
    if (text.empty() || text.size() > 24) return false;
    std::uint64_t cache = 0, data = 0;
    if (!parse_hex(text.substr(0, std::min<std::size_t>(16, text.size())), cache)) return false;
    if (text.size() > 16 && !parse_hex(text.substr(16), data)) return false;
    key.cache = cache;
    key.data = static_cast<std::uint32_t>(data);
    return true;
}

// Names leaving the pack ("..") are refused: packs are downloads.
bool escapes(std::string_view name) {
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t end = name.find_first_of("/\\", start);
        const std::string_view part = name.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (part == "..") return true;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return !name.empty() && (name.front() == '/' || name.front() == '\\' || (name.size() > 1 && name[1] == ':'));
}

std::uint32_t data_hash(const std::uint8_t *data, std::uint32_t size, HashKind kind, bool aligned16) {
    switch (kind) {
    case HashKind::Quick: return quick_hash(data, size, aligned16);
    case HashKind::Xxh32: return XXH32(data, size, kDataSeed);
    case HashKind::Xxh64: return static_cast<std::uint32_t>(XXH64(data, size, kDataSeed));
    }
    return 0;
}

// The region a release belongs to, from its product code: "eu", "us", "jp" or "".
std::string region_of(const std::string &game_id) {
    const std::string id = lower(game_id);
    if (id.rfind("ules", 0) == 0 || id.rfind("uces", 0) == 0 || id.rfind("npef", 0) == 0 || id.rfind("npeh", 0) == 0) return "eu";
    if (id.rfind("ulus", 0) == 0 || id.rfind("ucus", 0) == 0 || id.rfind("npuh", 0) == 0 || id.rfind("npuf", 0) == 0) return "us";
    if (id.rfind("uljm", 0) == 0 || id.rfind("uljs", 0) == 0 || id.rfind("npjh", 0) == 0 || id.rfind("npjg", 0) == 0) return "jp";
    return {};
}

// Whether an ini's name says it is for `region` (textures_FU_EU.ini, textures-us.ini, textures_JP.ini...).
bool named_for(const fs::path &ini, const std::string &region) {
    if (region.empty()) return false;
    const std::string stem = lower(ini.stem().string());
    std::vector<std::string> parts;
    std::string part;
    for (const char c : stem) {
        if (c == '_' || c == '-' || c == ' ' || c == '.') {
            if (!part.empty()) parts.push_back(part);
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    if (!part.empty()) parts.push_back(part);
    for (const std::string &p : parts) {
        if (p == region) return true;
        if (region == "jp" && (p == "ja" || p == "jpn" || p == "japan")) return true;
        if (region == "us" && (p == "usa" || p == "na")) return true;
        if (region == "eu" && (p == "eur" || p == "europe" || p == "pal")) return true;
    }
    return false;
}

// Sections and their lines, in file order (a key may appear more than once).
struct Ini {
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> sections;
    const std::vector<std::pair<std::string, std::string>> *section(const std::string &name) const {
        for (const auto &s : sections) {
            if (s.first == name) return &s.second;
        }
        return nullptr;
    }
    std::string get(const std::string &section_name, const std::string &key, bool &found) const {
        found = false;
        std::string value;
        if (const auto *s = section(section_name)) {
            for (const auto &[k, v] : *s) {
                if (lower(k) == lower(key)) {
                    found = true;
                    value = v;
                }
            }
        }
        return value;
    }
};

bool read_ini(const fs::path &path, Ini &ini) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        std::string_view v = line;
        if (first) {
            first = false;
            if (v.substr(0, 3) == "\xEF\xBB\xBF") v.remove_prefix(3);
        }
        v = trim(v);
        if (v.empty() || v.front() == '#' || v.front() == ';') continue;
        if (v.front() == '[') {
            const auto end = v.find(']');
            ini.sections.push_back({lower(trim(v.substr(1, end == std::string_view::npos ? std::string_view::npos : end - 1))), {}});
            continue;
        }
        if (ini.sections.empty()) ini.sections.push_back({std::string(), {}});
        const auto eq = v.find('=');
        std::string_view key = trim(eq == std::string_view::npos ? v : v.substr(0, eq));
        std::string_view value = eq == std::string_view::npos ? std::string_view() : trim(v.substr(eq + 1));
        // A comment after the value ("a.png # the sky"), and quotes around it, are not part of it.
        if (const auto hash = value.find(" #"); hash != std::string_view::npos) value = trim(value.substr(0, hash));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        ini.sections.back().second.emplace_back(std::string(key), std::string(value));
    }
    return true;
}

} // namespace

std::string utf8(const fs::path &path) {
    const std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

fs::path from_utf8(const std::string &text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string format_key(const Key &key) {
    char text[32];
    std::snprintf(text, sizeof text, "%016llx%08x", static_cast<unsigned long long>(key.cache), static_cast<unsigned>(key.data));
    return text;
}

// PPSSPP's quick hash, as ibonplix wrote it for MHForever: 64-byte blocks in eight 16-bit lanes when the data is
// aligned and a whole number of blocks, else pairs of 32-bit words. Little-endian hosts only, as the PSP's data is.
std::uint32_t quick_hash(const std::uint8_t *p, std::uint32_t size, bool aligned16) {
    std::uint32_t check = 0;
    if (aligned16 && (size & 0x3Fu) == 0) {
        std::uint16_t c[8] = {};
        std::uint16_t c2[8] = {0xc00b, 0x9bd9, 0x4b73, 0xb651, 0x4d9b, 0x4309, 0x0083, 0x0001};
        for (std::uint32_t off = 0; off < size; off += 64) {
            std::uint16_t v0[8], v3[8];
            std::uint32_t v1[4], v2[4], c32[4];
            std::memcpy(v0, p + off, 16);
            std::memcpy(v1, p + off + 16, 16);
            std::memcpy(v2, p + off + 32, 16);
            std::memcpy(v3, p + off + 48, 16);
            for (int j = 0; j < 8; ++j) c[j] = static_cast<std::uint16_t>(c[j] + static_cast<std::uint16_t>(std::uint32_t(v0[j]) * c2[j]));
            std::memcpy(c32, c, 16);
            for (int j = 0; j < 4; ++j) {
                c32[j] ^= v1[j];
                c32[j] += v2[j];
            }
            std::memcpy(c, c32, 16);
            for (int j = 0; j < 8; ++j) c[j] ^= static_cast<std::uint16_t>(std::uint32_t(v3[j]) * c2[j]);
            for (int j = 0; j < 8; ++j) c2[j] = static_cast<std::uint16_t>(c2[j] + 0x2455u);
        }
        std::uint32_t a[4], b[4];
        std::memcpy(a, c, 16);
        std::memcpy(b, c2, 16);
        for (int j = 0; j < 4; ++j) check += a[j] + b[j];
    } else {
        for (std::uint32_t i = 0; i < size / 8; ++i) {
            std::uint32_t x, y;
            std::memcpy(&x, p + i * 8, 4);
            std::memcpy(&y, p + i * 8 + 4, 4);
            check += x;
            check ^= y;
        }
    }
    return check;
}

std::vector<std::string> games_of(const fs::path &ini_path) {
    Ini ini;
    std::vector<std::string> games;
    if (!read_ini(ini_path, ini)) return games;
    if (const auto *s = ini.section("games")) {
        for (const auto &[k, v] : *s) games.push_back(k);
    }
    return games;
}

fs::path Pack::locate(const fs::path &chosen, const std::string &game_id) {
    std::error_code ec;
    const auto has_ini = [&](const fs::path &dir) {
        for (const auto &e : fs::directory_iterator(dir, ec)) {
            if (e.is_regular_file(ec) && lower(e.path().extension().string()) == ".ini" &&
                lower(e.path().filename().string()).rfind("textures", 0) == 0) {
                return true;
            }
        }
        return false;
    };
    if (!fs::is_directory(chosen, ec)) return {};
    if (has_ini(chosen)) return chosen;
    // PPSSPP's memory stick or its PSP folder: PSP/TEXTURES/<game id>.
    if (!game_id.empty()) {
        for (const fs::path &p : {chosen / "PSP" / "TEXTURES" / game_id, chosen / "TEXTURES" / game_id, chosen / game_id}) {
            if (fs::is_directory(p, ec) && has_ini(p)) return p;
        }
    }
    // A pack inside the chosen folder (KHD keeps it in MAIN, next to ADDONS): the one sub-folder holding an ini, or
    // the one named MAIN.
    std::vector<fs::path> found;
    for (const auto &e : fs::directory_iterator(chosen, ec)) {
        if (e.is_directory(ec) && has_ini(e.path())) found.push_back(e.path());
    }
    if (found.size() == 1) return found.front();
    for (const fs::path &p : found) {
        if (lower(p.filename().string()) == "main") return p;
    }
    return {};
}

std::unique_ptr<Pack> Pack::open(const fs::path &folder, const std::string &game_id, std::string &error) {
    std::unique_ptr<Pack> pack(new Pack);
    pack->folder_ = folder;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        error = "The texture pack folder is missing: " + folder.string();
        return nullptr;
    }
    if (lower(folder.extension().string()) == ".zip") {
        error = "Zipped texture packs are not read: unzip it first.";
        return nullptr;
    }

    // Which ini: textures.ini, with the override [games] gives the release; or, when the pack has one ini per
    // release instead (KHD: textures.ini for Japan, textures_FU_US.ini, textures_FU_EU.ini), the one named for the
    // region of the release.
    std::vector<fs::path> inis;
    for (const auto &e : fs::directory_iterator(folder, ec)) {
        if (e.is_regular_file(ec) && lower(e.path().extension().string()) == ".ini") inis.push_back(e.path());
    }
    std::sort(inis.begin(), inis.end());
    fs::path base;
    for (const fs::path &p : inis) {
        if (lower(p.filename().string()) == "textures.ini") base = p;
    }
    const std::string region = region_of(game_id);
    fs::path regional;
    for (const fs::path &p : inis) {
        if (p != base && named_for(p, region)) {
            regional = p;
            break;
        }
    }
    std::string override_name;
    if (!base.empty() && !game_id.empty()) {
        Ini ini;
        read_ini(base, ini);
        if (const auto *s = ini.section("games")) {
            for (const auto &[k, v] : *s) {
                if (lower(k) == lower(game_id)) override_name = v.empty() ? "true" : v;
            }
            if (override_name.empty() && !s->empty() && regional.empty()) {
                pack->warnings_.push_back("textures.ini lists other releases in [games], not " + game_id);
            }
        }
    }

    if (!override_name.empty() || regional.empty()) {
        if (!base.empty()) {
            if (!pack->parse(base, error)) return nullptr;
            pack->ini_ = base;
            if (!override_name.empty() && lower(override_name) != "true" && lower(override_name) != "textures.ini") {
                if (escapes(override_name)) {
                    error = "The override ini in [games] must stay inside the pack: " + override_name;
                    return nullptr;
                }
                const fs::path extra = folder / override_name;
                if (!pack->parse(extra, error)) return nullptr;
                pack->ini_ = extra;
            }
        } else if (inis.empty()) {
            pack->warnings_.push_back("No textures.ini: only images named by their key in the pack's top folder are used");
        } else {
            error = "None of the pack's ini files is for this release (" + game_id + ")";
            return nullptr;
        }
    } else {
        if (!pack->parse(regional, error)) return nullptr;
        pack->ini_ = regional;
    }
    pack->add_named_images();
    if (pack->entries_.empty()) {
        error = "The texture pack names no texture";
        return nullptr;
    }
    return pack;
}

std::unique_ptr<Pack> Pack::keys_only(const Options &options) {
    std::unique_ptr<Pack> pack(new Pack);
    pack->options_ = options;
    return pack;
}

bool Pack::parse(const fs::path &path, std::string &error) {
    Ini ini;
    if (!read_ini(path, ini)) {
        error = "Cannot read " + path.filename().string();
        return false;
    }
    const bool first = entries_.empty() && ini_.empty();
    bool found = false;
    const std::string hash = lower(ini.get("options", "hash", found));
    if (found && !hash.empty()) {
        if (hash == "quick") options_.hash = HashKind::Quick;
        else if (hash == "xxh32") options_.hash = HashKind::Xxh32;
        else if (hash == "xxh64") options_.hash = HashKind::Xxh64;
        else {
            error = path.filename().string() + ": hash = " + hash + " is not supported (quick, xxh32 and xxh64 are)";
            return false;
        }
    } else if (first && !found) {
        warnings_.push_back(path.filename().string() + " does not say which hash it uses: quick is assumed");
    }
    std::string v = ini.get("options", "ignoreAddress", found);
    if (found) options_.ignore_address = parse_bool(v);
    v = ini.get("options", "reduceHash", found);
    if (found) options_.reduce_hash = parse_bool(v);
    v = ini.get("options", "reduceHashGlobalValue", found);
    if (found) parse_number(v, options_.reduce_hash_default);
    v = ini.get("options", "skipLastDXT1Blocks128x64", found);
    if (found) parse_number(v, options_.skip_last_dxt1_blocks_128x64);
    v = ini.get("options", "skipLastDXT1Blocks128x128", found);
    if (found) parse_number(v, options_.skip_last_dxt1_blocks_128x128);
    v = ini.get("options", "version", found);
    if (found) parse_number(v, options_.version);
    // As PPSSPP: the quick hash covers too little to leave the address out or hash only part of a texture.
    if (options_.hash == HashKind::Quick && options_.ignore_address) {
        options_.ignore_address = false;
        warnings_.push_back("ignoreAddress needs xxh32 or xxh64: turned off");
    }
    if (options_.hash == HashKind::Quick && options_.reduce_hash) {
        options_.reduce_hash = false;
        warnings_.push_back("reduceHash needs xxh32 or xxh64: turned off");
    }

    // [hashes]: key = image (or images: "a.png|a_1.png" is not the ini's syntax; levels are separate keys "_1").
    std::map<std::pair<std::uint64_t, std::uint32_t>, std::map<int, std::string>> levels;
    std::size_t bad = 0;
    if (const auto *s = ini.section("hashes")) {
        for (const auto &[k, value] : *s) {
            Key key;
            int level = 0;
            if (!parse_key(k, key, level)) {
                ++bad;
                continue;
            }
            if (escapes(value)) {
                warnings_.push_back("Ignored an image outside the pack: " + value);
                continue;
            }
            std::string name = value;
            std::replace(name.begin(), name.end(), '\\', '/');
            levels[{key.cache, key.data}][level] = name;
        }
    }
    if (bad != 0) warnings_.push_back(std::to_string(bad) + " lines under [hashes] are not keys and were skipped");
    for (const auto &[k, by_level] : levels) {
        Entry entry;
        // Mip levels count only while they follow one another from 0.
        int next = 0;
        for (const auto &[level, name] : by_level) {
            if (level != next) break;
            entry.levels[level] = name;
            ++next;
        }
        entries_[Key{k.first, k.second}] = std::move(entry); // a later ini (an override) wins
    }

    if (const auto *s = ini.section("hashranges")) {
        for (const auto &[k, value] : *s) {
            // addr,w,h = w,h
            const auto c1 = k.find(','), c2 = k.find(',', c1 == std::string::npos ? c1 : c1 + 1);
            const auto c3 = value.find(',');
            std::uint32_t addr = 0, from_w = 0, from_h = 0, to_w = 0, to_h = 0;
            std::string a = k.substr(0, c1);
            if (a.rfind("0x", 0) != 0 && a.rfind("0X", 0) != 0) a = "0x" + a;
            if (c1 == std::string::npos || c2 == std::string::npos || c3 == std::string::npos || !parse_number(a, addr) ||
                !parse_number(std::string_view(k).substr(c1 + 1, c2 - c1 - 1), from_w) ||
                !parse_number(std::string_view(k).substr(c2 + 1), from_h) ||
                !parse_number(std::string_view(value).substr(0, c3), to_w) || !parse_number(std::string_view(value).substr(c3 + 1), to_h) ||
                to_w > from_w || to_h > from_h || to_w == 0 || to_h == 0) {
                warnings_.push_back("Ignored the hash range " + k + " = " + value);
                continue;
            }
            hash_ranges_[std::uint64_t(addr) << 32 | std::uint64_t(from_w) << 16 | from_h] = {to_w, to_h};
        }
    }
    if (const auto *s = ini.section("reducehashranges")) {
        for (const auto &[k, value] : *s) {
            const auto c = k.find(',');
            std::uint32_t w = 0, h = 0;
            float factor = 0.0f;
            if (c == std::string::npos || !parse_number(std::string_view(k).substr(0, c), w) ||
                !parse_number(std::string_view(k).substr(c + 1), h) || !parse_number(value, factor) || factor == 0.0f) {
                warnings_.push_back("Ignored the reduce hash range " + k + " = " + value);
                continue;
            }
            reduce_ranges_[w << 16 | h] = factor;
        }
    }
    if (const auto *s = ini.section("filtering")) {
        for (const auto &[k, value] : *s) {
            Key key;
            int level = 0;
            const std::string f = lower(value);
            if (!parse_key(k, key, level) || (f != "nearest" && f != "linear" && f != "auto")) {
                warnings_.push_back("Ignored the filtering line " + k + " = " + value);
                continue;
            }
            filters_[key] = f == "nearest" ? Filter::Nearest : f == "linear" ? Filter::Linear : Filter::Auto;
        }
    }
    if (const auto *s = ini.section("games")) {
        for (const auto &[k, value] : *s) games_.push_back(k);
    }
    if (ini.section("indexed") != nullptr) warnings_.push_back("[indexed] (palette images) is not supported: those textures use their full images");
    return true;
}

// PPSSPP also takes images in the pack's top folder named by their key ("<key>.png", "<key>_1.png" for a mip level)
// when the ini does not list them.
void Pack::add_named_images() {
    std::error_code ec;
    std::map<std::pair<std::uint64_t, std::uint32_t>, std::map<int, std::string>> found;
    for (const auto &e : fs::directory_iterator(folder_, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string name = e.path().filename().string();
        const std::string ext = lower(e.path().extension().string());
        if (name.empty() || name.front() == '.' || (ext != ".png" && ext != ".ktx2" && ext != ".dds" && ext != ".zim")) continue;
        const std::string stem = e.path().stem().string();
        if (!(stem.size() == 24 || (stem.size() >= 26 && stem.size() <= 27 && stem[24] == '_'))) continue;
        Key key;
        int level = 0;
        if (parse_key(stem, key, level)) found[{key.cache, key.data}][level] = name;
    }
    for (const auto &[k, by_level] : found) {
        if (entries_.count(Key{k.first, k.second}) != 0) continue;
        Entry entry;
        int next = 0;
        for (const auto &[level, name] : by_level) {
            if (level != next) break;
            entry.levels[level] = name;
            ++next;
        }
        entries_[Key{k.first, k.second}] = std::move(entry);
    }
}

bool Pack::key_of(const TextureFacts &t, const MemoryReader &memory, Key &key, std::uint32_t &covered_width,
                  std::uint32_t &covered_height) const {
    if (t.format > 10 || t.width == 0 || t.height == 0) return false;
    const std::uint32_t bits = kBitsPerTexel[t.format];
    // The buffer width as PPSSPP reads it: whole 16-byte units for the plain and CLUT formats, at least one unit.
    std::uint32_t buffer_width = t.buffer_width & (t.format <= 7 ? (0x7FFu & ~((128u / bits) - 1u)) : 0x7FFu);
    if (buffer_width == 0) buffer_width = 128u / bits;

    std::uint32_t w = t.width, h = t.height;
    covered_width = t.width;
    covered_height = t.height;
    if (const auto range = hash_ranges_.find(std::uint64_t(t.address) << 32 | std::uint64_t(w) << 16 | h); range != hash_ranges_.end()) {
        // The pack replaces only this part of the texture.
        w = range->second.first;
        h = range->second.second;
        covered_width = w;
        covered_height = h;
    } else if (h == 512 && t.max_seen_v != 0 && t.max_seen_v < 512) {
        // 512-tall textures drawn in through mode are hashed down to the lowest row drawn.
        h = t.max_seen_v;
    }

    float reduce = 1.0f;
    if (options_.reduce_hash) {
        const auto r = reduce_ranges_.find(w << 16 | h);
        reduce = r != reduce_ranges_.end() ? r->second : options_.reduce_hash_default;
    }
    reduce = std::clamp(reduce, 0.0f, 1.0f);

    std::uint32_t hash = 0;
    if (buffer_width <= w) {
        // The rows follow one another: the texels used, as one run.
        const std::uint32_t texels = buffer_width * h + (w - buffer_width);
        std::uint32_t size = static_cast<std::uint32_t>(static_cast<float>(bits * texels / 8u) * reduce);
        if (t.format == 8 && w == 128 && h == 64) size -= std::min(size, 8u * options_.skip_last_dxt1_blocks_128x64);
        if (t.format == 8 && w == 128 && h == 128) size -= std::min(size, 8u * options_.skip_last_dxt1_blocks_128x128);
        const std::uint8_t *data = memory(t.address, size);
        if (data == nullptr) return false;
        hash = data_hash(data, size, options_.hash, (t.address & 0xFu) == 0);
    } else {
        // Rows narrower than the buffer: each row hashed on its own, folded together.
        const std::uint32_t row_bytes = static_cast<std::uint32_t>(static_cast<float>(bits * w / 8u) * reduce);
        const std::uint32_t stride = bits * buffer_width / 8u;
        const std::uint32_t span = h > 0 ? (h - 1) * stride + row_bytes : 0;
        const std::uint8_t *data = memory(t.address, span);
        if (data == nullptr) return false;
        for (std::uint32_t y = 0; y < h; ++y) {
            const std::uint32_t row = data_hash(data + std::size_t(y) * stride, row_bytes, options_.hash, ((t.address + y * stride) & 0xFu) == 0);
            hash = hash * 11u ^ row;
        }
    }

    std::uint64_t cache = std::uint64_t(t.address & 0x3FFFFFFFu) << 32 | t.dimension;
    if (t.format >= 4 && t.format <= 7) {
        // The palette: as the last load left it, plus the entries the index offset reaches past it, up to the most
        // any load wrote; the CLUT format command word, command byte and all, is mixed in.
        const std::uint32_t entry_bytes = (t.clut_format_word & 3u) == 3u ? 4u : 2u;
        const std::uint32_t base_bytes = ((t.clut_format_word >> 16u) & 0x1Fu) * 16u * entry_bytes;
        const std::uint32_t clut_bytes = std::min(t.clut_last_bytes + base_bytes, t.clut_max_bytes);
        std::uint32_t clut_hash = XXH32(t.clut, t.clut != nullptr ? clut_bytes : 0u, kClutSeed);
        clut_hash ^= t.clut_format_word;
        cache ^= clut_hash;
    }
    if (options_.ignore_address) cache &= 0xFFFFFFFFull;
    key.cache = cache;
    key.data = hash;
    return true;
}

// PPSSPP's order: the full key; the palette alone; (with addresses) the address and palette without the data; no
// address; the address without the palette; the address alone; then the data hash alone.
template <typename T>
const T *Pack::lookup(const std::unordered_map<Key, T, KeyHash> &map, const Key &key) const {
    if (map.empty()) return nullptr;
    const auto at = [&](std::uint64_t cache, std::uint32_t data) -> const T * {
        const auto it = map.find(Key{cache, data});
        return it == map.end() ? nullptr : &it->second;
    };
    const std::uint64_t low = key.cache & 0xFFFFFFFFull, high = key.cache & ~0xFFFFFFFFull;
    if (const T *v = at(key.cache, key.data)) return v;
    if (const T *v = at(low, 0)) return v;
    if (!options_.ignore_address) {
        if (const T *v = at(key.cache, 0)) return v;
    }
    if (const T *v = at(low, key.data)) return v;
    if (!options_.ignore_address) {
        if (const T *v = at(high, key.data)) return v;
        if (const T *v = at(high, 0)) return v;
    }
    return at(0, key.data);
}

Match Pack::find(const Key &key) const {
    Match m;
    m.entry = lookup(entries_, key);
    if (m.entry == nullptr) return m;
    if (const Filter *f = lookup(filters_, key)) m.filter = *f;
    else if (const auto any = filters_.find(Key{0, 0}); any != filters_.end()) m.filter = any->second;
    return m;
}

fs::path Pack::path_of(const std::string &image) const {
    fs::path p = folder_ / from_utf8(image);
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) return p;
    std::call_once(folded_once_, [this] {
        std::error_code e;
        for (auto it = fs::recursive_directory_iterator(folder_, e); it != fs::recursive_directory_iterator(); it.increment(e)) {
            if (e) break;
            if (!it->is_regular_file(e)) continue;
            const std::u8string r = fs::relative(it->path(), folder_, e).generic_u8string();
            const std::string rel(r.begin(), r.end());
            folded_names_.emplace(lower(rel), it->path());
        }
    });
    std::string wanted = lower(image);
    std::replace(wanted.begin(), wanted.end(), '\\', '/');
    if (const auto it = folded_names_.find(wanted); it != folded_names_.end()) return it->second;
    return p;
}

bool Pack::review(const fs::path &folder, const std::string &game_id, Review &review, std::string &error) {
    review = {};
    const std::unique_ptr<Pack> pack = open(folder, game_id, error);
    if (!pack) return false;
    review.folder = pack->folder_;
    review.ini = pack->ini_;
    review.hash = pack->options_.hash == HashKind::Quick ? "quick" : pack->options_.hash == HashKind::Xxh32 ? "xxh32" : "xxh64";
    review.keys = pack->entries_.size();
    review.games = pack->games_;
    review.warnings = pack->warnings_;
    std::set<std::string> images;
    for (const auto &[k, entry] : pack->entries_) {
        for (const auto &[level, name] : entry.levels) {
            if (!name.empty()) images.insert(name);
        }
    }
    review.images = images.size();
    std::error_code ec;
    for (const std::string &name : images) {
        const fs::path p = pack->path_of(name);
        const auto size = fs::file_size(p, ec);
        if (ec) {
            ++review.missing;
            ec.clear();
        } else {
            review.bytes += size;
        }
    }
    return true;
}

} // namespace mhf::psp::textures
