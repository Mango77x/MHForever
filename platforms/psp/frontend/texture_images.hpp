// The images of texture packs: decoding them (KTX2 with Basis Universal UASTC or ETC1S, as PPSSPP 1.15+ packs use, and
// PNG) and doing it on threads of their own, so the game never waits for a texture: until its image is ready the
// game's own texture is drawn (texture_pack.hpp has the packs themselves).

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mhf::psp::textures {

// An image in RGBA8, with the mip levels its files had (level 0 first, each half the size of the one before).
struct Image {
    std::uint32_t width = 0, height = 0;
    std::vector<std::vector<std::uint8_t>> levels;
    [[nodiscard]] std::size_t bytes() const {
        std::size_t n = 0;
        for (const auto &l : levels) n += l.size();
        return n;
    }
};

// Decodes one file (all its levels: a KTX2 file may hold mips). False with `error` set when it cannot.
bool decode_image(const std::filesystem::path &path, Image &image, std::string &error);
// Decodes an entry's files: the first is level 0, each next one a mip level (kept while it halves the size).
bool decode_levels(const std::vector<std::filesystem::path> &files, Image &image, std::string &error);
// Writes RGBA8 pixels as a PNG (texture dumps for pack makers).
bool write_png(const std::filesystem::path &path, std::uint32_t width, std::uint32_t height, const std::uint32_t *rgba);

// Decodes images on background threads. get() never blocks: it queues what it has not seen and returns the image once
// it is decoded. The backend takes it (forget) once it is on the GPU; decoded images nobody took are dropped, oldest
// first, past `ready_budget` bytes.
class Loader {
public:
    explicit Loader(unsigned threads = 2, std::size_t ready_budget = 512u << 20);
    ~Loader();
    Loader(const Loader &) = delete;
    Loader &operator=(const Loader &) = delete;

    enum class State { Missing, Loading, Ready, Failed };
    // The image named `id` (made of `files`): Ready with it set, Loading (queued now if new), or Failed.
    State get(const std::string &id, const std::vector<std::filesystem::path> &files, std::shared_ptr<const Image> &image);
    // Drops the decoded copy of `id` (it is on the GPU now; asking again decodes it again).
    void forget(const std::string &id);
    // Incremented whenever an image finishes, so callers poll only when something changed.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }
    // Drops everything not being decoded and the queue (the pack was turned off).
    void clear();
    // Images decoded and failed since the start, and the time spent decoding (for logs).
    struct Counters {
        std::uint64_t decoded = 0, failed = 0, decode_us = 0, bytes = 0;
    };
    [[nodiscard]] Counters counters() const;

private:
    struct Job {
        std::string id;
        std::vector<std::filesystem::path> files;
    };
    struct Slot {
        State state = State::Loading;
        std::shared_ptr<const Image> image;
        std::list<std::string>::iterator order; // among the ready ones, oldest first
    };
    void run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    std::unordered_map<std::string, Slot> slots_;
    std::list<std::string> ready_order_;
    std::size_t ready_bytes_ = 0, ready_budget_;
    std::atomic<std::uint64_t> generation_{0};
    Counters counters_;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};

} // namespace mhf::psp::textures
