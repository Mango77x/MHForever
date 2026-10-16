#include "mhf/psp/frame_dump.hpp"

#include "kernel.hpp"

#include "mhf/psp/host.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace mhf::psp {

namespace {

std::uint32_t crc32(const std::uint8_t *data, std::size_t size, std::uint32_t crc = 0) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1u) : c >> 1u;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8u);
    return ~crc;
}

void put32be(std::vector<std::uint8_t> &out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

void chunk(std::vector<std::uint8_t> &png, const char *type, const std::vector<std::uint8_t> &data) {
    put32be(png, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    png.insert(png.end(), body.begin(), body.end());
    put32be(png, crc32(body.data(), body.size()));
}

// RGB8 image → PNG with stored (uncompressed) deflate blocks.
std::vector<std::uint8_t> encode_png(const std::vector<std::uint8_t> &rgb, unsigned width, unsigned height) {
    std::vector<std::uint8_t> raw;
    raw.reserve((width * 3 + 1) * height);
    for (unsigned y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + static_cast<long>(y * width * 3), rgb.begin() + static_cast<long>((y + 1) * width * 3));
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (const auto byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    for (std::size_t pos = 0; pos < raw.size(); pos += 65535) {
        const auto n = static_cast<std::uint16_t>(std::min<std::size_t>(65535, raw.size() - pos));
        z.push_back(pos + n >= raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8u));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8u));
        z.insert(z.end(), raw.begin() + static_cast<long>(pos), raw.begin() + static_cast<long>(pos + n));
    }
    put32be(z, (b << 16u) | a);
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    put32be(ihdr, width);
    put32be(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

std::uint32_t expand(std::uint32_t v, unsigned bits) {
    return bits == 5 ? (v << 3u) | (v >> 2u) : bits == 6 ? (v << 2u) | (v >> 4u) : v | (v << 4u);
}

} // namespace

bool dump_display(Runtime &rt, const std::filesystem::path &path) {
    const auto &d = display_state(rt);
    if (d.framebuffer == 0) {
        return false;
    }
    constexpr unsigned kWidth = 480, kHeight = 272;
    std::vector<std::uint8_t> rgb(kWidth * kHeight * 3);
    auto &mem = rt.memory();
    for (unsigned y = 0; y < kHeight; ++y) {
        for (unsigned x = 0; x < kWidth; ++x) {
            const std::uint32_t i = y * d.stride + x;
            std::uint32_t r, g, b;
            if (d.pixel_format == 3) {
                const std::uint32_t p = mem.load32(d.framebuffer + i * 4u);
                r = p & 0xFFu; g = (p >> 8u) & 0xFFu; b = (p >> 16u) & 0xFFu;
            } else {
                const std::uint32_t p = mem.load16(d.framebuffer + i * 2u);
                if (d.pixel_format == 0) { r = expand(p & 31u, 5); g = expand((p >> 5u) & 63u, 6); b = expand((p >> 11u) & 31u, 5); }
                else if (d.pixel_format == 1) { r = expand(p & 31u, 5); g = expand((p >> 5u) & 31u, 5); b = expand((p >> 10u) & 31u, 5); }
                else { r = expand(p & 15u, 4); g = expand((p >> 4u) & 15u, 4); b = expand((p >> 8u) & 15u, 4); }
            }
            const std::size_t o = (y * kWidth + x) * 3u;
            rgb[o] = static_cast<std::uint8_t>(r);
            rgb[o + 1] = static_cast<std::uint8_t>(g);
            rgb[o + 2] = static_cast<std::uint8_t>(b);
        }
    }
    const auto png = encode_png(rgb, kWidth, kHeight);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
    return true;
}

void enable_frame_dumps(Runtime &rt, const std::filesystem::path &folder, unsigned every) {
    std::filesystem::create_directories(folder);
    auto *runtime = &rt;
    auto last = std::make_shared<std::uint64_t>(0);
    auto count = std::make_shared<unsigned>(0);
    rt.kernel().host_poll = [runtime, folder, every, last, count]() {
        const std::uint64_t vblank = runtime->kernel().now_us() / kVblankPeriodUs;
        if (vblank == *last) return;
        *last = vblank;
        if (vblank % (every == 0 ? 1 : every) != 0) return;
        char name[32];
        std::snprintf(name, sizeof name, "frame_%06u.png", ++*count);
        dump_display(*runtime, folder / name);
    };
}

} // namespace mhf::psp
