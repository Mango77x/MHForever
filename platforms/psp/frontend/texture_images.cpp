// Texture pack images: decoding and the background loader (texture_images.hpp).

#include "texture_images.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "basisu_transcoder.h"
#include "texture_pack.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace mhf::psp::textures {
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kMaxSide = 8192;

bool read_file(const fs::path &path, std::vector<std::uint8_t> &data, std::string &error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        error = "cannot open " + utf8(path);
        return false;
    }
    const std::streamsize n = in.tellg();
    if (n <= 0 || n > (std::streamsize(1) << 30)) {
        error = "unexpected size of " + utf8(path);
        return false;
    }
    data.resize(static_cast<std::size_t>(n));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char *>(data.data()), n)) {
        error = "cannot read " + utf8(path);
        return false;
    }
    return true;
}

bool decode_ktx2(const std::vector<std::uint8_t> &data, Image &image, std::string &error) {
    static std::once_flag once;
    std::call_once(once, [] { basist::basisu_transcoder_init(); });
    basist::ktx2_transcoder tr;
    if (!tr.init(data.data(), static_cast<std::uint32_t>(data.size()))) {
        error = "not a KTX2 file Basis Universal can read";
        return false;
    }
    if (tr.get_faces() != 1 || tr.get_layers() > 1) {
        error = "a cube map or an array texture";
        return false;
    }
    const std::uint32_t w = tr.get_width(), h = tr.get_height();
    if (w == 0 || h == 0 || w > kMaxSide || h > kMaxSide) {
        error = "unexpected size " + std::to_string(w) + "x" + std::to_string(h);
        return false;
    }
    if (!tr.start_transcoding()) {
        error = "cannot start transcoding";
        return false;
    }
    image.width = w;
    image.height = h;
    image.levels.clear();
    const std::uint32_t levels = std::max(1u, tr.get_levels());
    for (std::uint32_t level = 0; level < levels; ++level) {
        basist::ktx2_image_level_info info;
        if (!tr.get_image_level_info(info, level, 0, 0)) break;
        const std::uint32_t lw = info.m_orig_width, lh = info.m_orig_height;
        if (level > 0 && (lw != std::max(1u, w >> level) || lh != std::max(1u, h >> level))) break;
        std::vector<std::uint8_t> rgba(std::size_t(lw) * lh * 4u);
        // An uncompressed output's size is counted in pixels.
        if (!tr.transcode_image_level(level, 0, 0, rgba.data(), lw * lh, basist::transcoder_texture_format::cTFRGBA32, 0, lw, lh)) {
            if (level == 0) {
                error = "transcoding failed";
                return false;
            }
            break;
        }
        image.levels.push_back(std::move(rgba));
    }
    return !image.levels.empty();
}

bool decode_png(const std::vector<std::uint8_t> &data, Image &image, std::string &error) {
    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &channels) || w <= 0 || h <= 0 ||
        w > static_cast<int>(kMaxSide) || h > static_cast<int>(kMaxSide)) {
        error = "not a PNG image of a usable size";
        return false;
    }
    stbi_uc *pixels = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &channels, 4);
    if (pixels == nullptr) {
        error = std::string("PNG: ") + stbi_failure_reason();
        return false;
    }
    image.width = static_cast<std::uint32_t>(w);
    image.height = static_cast<std::uint32_t>(h);
    image.levels.assign(1, std::vector<std::uint8_t>(pixels, pixels + std::size_t(w) * h * 4u));
    stbi_image_free(pixels);
    return true;
}

std::string lower_extension(const fs::path &path) {
    std::string ext = path.extension().string();
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

} // namespace

bool decode_image(const fs::path &path, Image &image, std::string &error) {
    const std::string ext = lower_extension(path);
    if (ext != ".ktx2" && ext != ".png") {
        error = ext.empty() ? "an image without a type" : ext.substr(1) + " images are not supported (PNG and KTX2 are)";
        return false;
    }
    std::vector<std::uint8_t> data;
    if (!read_file(path, data, error)) return false;
    // By content, not by name: a mislabelled file still decodes.
    static constexpr std::uint8_t kKtx2[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    static constexpr std::uint8_t kPng[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (data.size() >= sizeof kKtx2 && std::memcmp(data.data(), kKtx2, sizeof kKtx2) == 0) return decode_ktx2(data, image, error);
    if (data.size() >= sizeof kPng && std::memcmp(data.data(), kPng, sizeof kPng) == 0) return decode_png(data, image, error);
    error = "neither a PNG nor a KTX2 file";
    return false;
}

bool decode_levels(const std::vector<fs::path> &files, Image &image, std::string &error) {
    if (files.empty()) {
        error = "no image";
        return false;
    }
    if (!decode_image(files.front(), image, error)) {
        error = utf8(files.front().filename()) + ": " + error;
        return false;
    }
    // Mip levels given as their own files (key_1, key_2...), kept while each halves the size.
    for (std::size_t i = 1; i < files.size() && image.levels.size() == i; ++i) {
        Image level;
        std::string ignored;
        if (!decode_image(files[i], level, ignored)) break;
        if (level.width != std::max(1u, image.width >> i) || level.height != std::max(1u, image.height >> i)) break;
        image.levels.push_back(std::move(level.levels.front()));
    }
    return true;
}

bool write_png(const fs::path &path, std::uint32_t width, std::uint32_t height, const std::uint32_t *rgba) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const auto sink = [](void *context, void *data, int size) {
        static_cast<std::ofstream *>(context)->write(static_cast<const char *>(data), size);
    };
    const int ok = stbi_write_png_to_func(sink, &out, static_cast<int>(width), static_cast<int>(height), 4, rgba,
                                          static_cast<int>(width * 4u));
    return ok != 0 && static_cast<bool>(out);
}

Loader::Loader(unsigned threads, std::size_t ready_budget) : ready_budget_(ready_budget) {
    threads = std::max(1u, threads);
    for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this] { run(); });
}

Loader::~Loader() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        queue_.clear();
    }
    wake_.notify_all();
    for (std::thread &t : threads_) t.join();
}

Loader::State Loader::get(const std::string &id, const std::vector<fs::path> &files, std::shared_ptr<const Image> &image) {
    std::lock_guard lock(mutex_);
    const auto it = slots_.find(id);
    if (it == slots_.end()) {
        slots_.emplace(id, Slot{});
        queue_.push_back(Job{id, files});
        wake_.notify_one();
        return State::Loading;
    }
    if (it->second.state == State::Ready) image = it->second.image;
    return it->second.state;
}

void Loader::forget(const std::string &id) {
    std::lock_guard lock(mutex_);
    const auto it = slots_.find(id);
    if (it == slots_.end() || it->second.state == State::Loading) return;
    if (it->second.state == State::Ready) {
        ready_bytes_ -= std::min(ready_bytes_, it->second.image->bytes());
        ready_order_.erase(it->second.order);
    }
    if (it->second.state != State::Failed) slots_.erase(it); // a failure is remembered: no retries
}

void Loader::clear() {
    std::lock_guard lock(mutex_);
    for (const Job &job : queue_) slots_.erase(job.id);
    queue_.clear();
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (it->second.state == State::Ready) it = slots_.erase(it);
        else ++it;
    }
    ready_order_.clear();
    ready_bytes_ = 0;
}

Loader::Counters Loader::counters() const {
    std::lock_guard lock(mutex_);
    return counters_;
}

void Loader::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        const auto start = std::chrono::steady_clock::now();
        auto image = std::make_shared<Image>();
        std::string error;
        const bool ok = decode_levels(job.files, *image, error);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        if (!ok) std::fprintf(stderr, "[textures] %s\n", error.c_str());
        {
            std::lock_guard lock(mutex_);
            counters_.decode_us += static_cast<std::uint64_t>(us);
            const auto it = slots_.find(job.id);
            if (it == slots_.end()) continue; // cleared meanwhile
            if (!ok) {
                it->second.state = State::Failed;
                ++counters_.failed;
            } else {
                it->second.state = State::Ready;
                it->second.image = image;
                ready_order_.push_back(job.id);
                it->second.order = std::prev(ready_order_.end());
                ready_bytes_ += image->bytes();
                ++counters_.decoded;
                counters_.bytes += image->bytes();
                // Past the budget, the oldest images nobody took are dropped (they decode again when asked).
                while (ready_bytes_ > ready_budget_ && ready_order_.size() > 1) {
                    const auto oldest = slots_.find(ready_order_.front());
                    ready_bytes_ -= std::min(ready_bytes_, oldest->second.image->bytes());
                    ready_order_.pop_front();
                    slots_.erase(oldest);
                }
            }
        }
        generation_.fetch_add(1, std::memory_order_acq_rel);
    }
}

} // namespace mhf::psp::textures
