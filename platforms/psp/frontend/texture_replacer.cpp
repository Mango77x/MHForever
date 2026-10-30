#include "texture_replacer.hpp"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string_view>
#include <system_error>

#include "basisu_transcoder.h"

namespace mhf::psp::frontend {
namespace {

constexpr std::size_t kCacheLimitBytes = 512u * 1024u * 1024u;

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool parse_hex(std::string_view s, std::uint64_t &out) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out, 16);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

TextureReplacer::Image decode_ktx2(const std::filesystem::path &path) {
    static std::once_flag once;
    std::call_once(once, [] { basist::basisu_transcoder_init(); });

    TextureReplacer::Image img;
    const std::string name = path.string();
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "[tex] cannot open %s\n", name.c_str());
        return img;
    }
    const std::streamsize n = in.tellg();
    in.seekg(0);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(n));
    if (n <= 0 || !in.read(reinterpret_cast<char *>(data.data()), n)) {
        std::fprintf(stderr, "[tex] cannot read %s\n", name.c_str());
        return img;
    }
    basist::ktx2_transcoder tr;
    if (!tr.init(data.data(), static_cast<std::uint32_t>(data.size())) || !tr.start_transcoding()) {
        std::fprintf(stderr, "[tex] not a usable KTX2 file: %s\n", name.c_str());
        return img;
    }
    const std::uint32_t w = tr.get_width(), h = tr.get_height();
    if (w == 0 || h == 0 || w > 8192 || h > 8192) {
        std::fprintf(stderr, "[tex] unexpected size %ux%u in %s\n", w, h, name.c_str());
        return img;
    }
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4u);
    // The output size of an uncompressed format is counted in pixels.
    if (!tr.transcode_image_level(0, 0, 0, rgba.data(), w * h, basist::transcoder_texture_format::cTFRGBA32)) {
        std::fprintf(stderr, "[tex] transcoding failed: %s\n", name.c_str());
        return img;
    }
    img.width = w;
    img.height = h;
    img.rgba = std::move(rgba);
    return img;
}

} // namespace

const TextureReplacer::Image *TextureReplacer::image(std::uint32_t hash) {
    if (const auto c = images_.find(hash); c != images_.end()) return c->second.rgba.empty() ? nullptr : &c->second;
    const std::filesystem::path *file = find(hash);
    if (!file) return nullptr;
    if (cache_bytes_ > kCacheLimitBytes) {
        images_.clear();
        cache_bytes_ = 0;
    }
    Image img = decode_ktx2(dir_ / *file);
    cache_bytes_ += img.rgba.size();
    const auto it = images_.emplace(hash, std::move(img)).first; // a failed load stays cached as empty: no retries
    return it->second.rgba.empty() ? nullptr : &it->second;
}

bool TextureReplacer::load(const std::filesystem::path &dir, const std::string &ini_name) {
    files_.clear();
    images_.clear();
    cache_bytes_ = 0;
    dir_ = dir;
    std::ifstream in(dir / ini_name);
    if (!in) {
        std::fprintf(stderr, "[tex] cannot open %s\n", (dir / ini_name).string().c_str());
        return false;
    }
    std::string line, section;
    bool quick = false, first = true;
    std::size_t skipped = 0, conflicts = 0;
    while (std::getline(in, line)) {
        std::string_view v = trim(line);
        if (first) {
            first = false;
            if (v.substr(0, 3) == "\xEF\xBB\xBF") v = trim(v.substr(3));
        }
        if (v.empty() || v.front() == '#' || v.front() == ';') continue;
        if (v.front() == '[') {
            const auto e = v.find(']');
            section = e == std::string_view::npos ? std::string() : std::string(v.substr(1, e - 1));
            continue;
        }
        const auto eq = v.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = trim(v.substr(0, eq)), val = trim(v.substr(eq + 1));
        if (section == "options") {
            if (key == "hash") quick = (val == "quick");
            continue;
        }
        if (section != "hashes") continue;
        // Key = 16 hex of cachekey + 8 hex of data hash. Length alone is not enough ("...2597 (2)" is 24 chars).
        std::uint64_t cache = 0, hash = 0;
        if (key.size() != 24 || !parse_hex(key.substr(0, 16), cache) || !parse_hex(key.substr(16), hash) || cache != 0 ||
            val.empty()) {
            ++skipped;
            continue;
        }
        const std::filesystem::path file{std::string(val)};
        const auto [it, inserted] = files_.emplace(static_cast<std::uint32_t>(hash), file);
        if (!inserted && it->second != file) ++conflicts;
    }
    if (!quick) {
        std::fprintf(stderr, "[tex] the pack does not declare 'hash = quick'; ignoring it\n");
        files_.clear();
        return false;
    }
    std::fprintf(stderr, "[tex] %zu replacements loaded, %zu lines skipped, %zu conflicting duplicates (first wins)\n",
                 files_.size(), skipped, conflicts);
    return true;
}

} // namespace mhf::psp::frontend
