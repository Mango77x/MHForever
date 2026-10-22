#pragma once

// PSP Graphics Engine: display-list processor and software renderer (docs/05-runtime-hle.md §6).
//
// The software renderer is the reference implementation: it draws straight into guest VRAM, so every framebuffer
// effect a game relies on (render to texture, block transfers, reading back) behaves like the hardware. A Vulkan
// backend will later replace the drawing part behind the same state.

#include "backend.hpp"
#include "psprecomp/guest_memory.hpp"

#include <array>
#include <cstring>
#include <cstdint>
#include <functional>
#include <vector>

namespace mhf::psp::gpu {

struct Matrix43 {
    std::array<float, 12> m{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}; // column-major 4x3 (as uploaded)
};
struct Matrix44 {
    std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

struct Vertex {
    float x{}, y{}, z{}, w{1};  // clip space (or screen space in through mode)
    float u{}, v{};             // texture coordinates (normalized, or texels in through mode)
    float r{}, g{}, b{}, a{};   // primary color 0..255
    float sr{}, sg{}, sb{};     // secondary (specular) color
    float fog{1};
    // For frame interpolation in backends: the position before the view matrix, and the guest address the vertex was
    // read from (0 when it was made up by clipping, or for lines and points).
    float world[3]{};
    bool has_world{};
    std::uint32_t source{};
};

// Called for SIGNAL and FINISH commands: (is_finish, value).
using ListEvent = std::function<void(bool finish, std::uint32_t value)>;

class Ge {
public:
    explicit Ge(psprecomp::GuestMemory &memory);

    struct ListResult {
        std::uint32_t pc{};
        bool ended{};
        bool stalled{};
    };
    // Executes commands from `pc` until END or until `stall` (0 = none) is reached.
    ListResult run(std::uint32_t pc, std::uint32_t stall, const ListEvent &on_event);

    // Statistics for logs and tests.
    struct Stats {
        std::uint64_t commands{}, primitives{}, vertices{}, pixels{}, transfers{};
        // Host time inside drawing commands: decoding and transforming vertices, and the rest (assembling primitives,
        // clipping, handing triangles to the backend).
        std::uint64_t transform_ns{}, assemble_ns{};
    };
    [[nodiscard]] const Stats &stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint32_t reg(std::uint32_t cmd) const noexcept { return regs_[cmd & 0xFF]; }

    // Hardware drawing: with a backend installed, rasterization goes there instead of guest VRAM.
    void set_backend(Backend *backend) noexcept { backend_ = backend; }
    [[nodiscard]] Backend *backend() const noexcept { return backend_; }

    // The current texture (level 0), for backends.
    struct TextureInfo {
        std::uint32_t address{}, stride{}, format{}, width{}, height{}, bytes{};
        bool swizzled{};
    };
    [[nodiscard]] TextureInfo texture_info() const;
    // Decodes level 0 into width*height ABGR8888 texels (CLUT, swizzling and DXT resolved).
    void decode_texture(std::uint32_t *out) const;
    // Hash of the texture bytes, and of the palette for indexed formats; changes when the texture would look different.
    [[nodiscard]] std::uint64_t texture_hash() const;
    // Incremented whenever the palette is reloaded.
    [[nodiscard]] std::uint64_t clut_generation() const noexcept { return clut_generation_; }
    // The current camera, for backends that place vertices again (frame interpolation).
    [[nodiscard]] const Matrix43 &view_matrix() const noexcept { return view_; }
    [[nodiscard]] const Matrix44 &proj_matrix() const noexcept { return proj_; }
    // Incremented whenever the view or projection matrix, the viewport or the screen offset is written.
    [[nodiscard]] std::uint64_t camera_generation() const noexcept { return camera_generation_; }

private:
    ListResult run_commands(std::uint32_t pc, std::uint32_t stall, const ListEvent &on_event);
    void execute(std::uint32_t word, std::uint32_t pc);
    void draw_primitive(std::uint32_t data);
    void draw_bezier_or_spline(std::uint32_t cmd, std::uint32_t data);
    void block_transfer(std::uint32_t data);
    void load_clut(std::uint32_t data);

    // Vertex pipeline.
    struct DecodedVertex {
        float pos[3]{}, uv[2]{}, color[4]{255, 255, 255, 255}, normal[3]{}, weights[8]{};
        bool has_color{};
        bool has_normal{};
    };
    [[nodiscard]] DecodedVertex decode_vertex(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t vertex_size() const;
    [[nodiscard]] Vertex transform(const DecodedVertex &in) const;
    void light(const float world_pos[3], const float world_normal[3], Vertex &out, const DecodedVertex &in) const;

    // Rasterizer.
    void draw_triangle(Vertex a, Vertex b, Vertex c);
    void draw_triangle_screen(const Vertex &a, const Vertex &b, const Vertex &c);
    void draw_sprite(const Vertex &a, const Vertex &b);
    void draw_line(const Vertex &a, const Vertex &b);
    void draw_point(const Vertex &p);
    [[nodiscard]] Vertex to_screen(const Vertex &clip) const;
    void shade_pixel(int x, int y, float z, float u, float v, const float color[4], const float secondary[3], float fog);

    // Textures.
    void sample_texture(float u, float v, float out[4]) const;
    [[nodiscard]] std::uint32_t fetch_texel(int level, int x, int y) const;
    [[nodiscard]] std::uint32_t clut_lookup(std::uint32_t index) const;

    // Framebuffer access.
    [[nodiscard]] std::uint32_t read_pixel(int x, int y) const;
    void write_pixel(int x, int y, std::uint32_t abgr, std::uint32_t write_mask);


    Backend *backend_{};
    std::uint64_t clut_generation_{};
    std::uint64_t camera_generation_{};

    // Direct access to guest RAM and EDRAM for the hot paths (vertices, texels, pixels): no bounds exceptions, an
    // address outside both reads 0 and ignores writes, like the open bus of the real GE would at worst.
    std::uint8_t *ram_{};
    std::uint32_t ram_size_{};
    std::uint8_t *vram_{};
    [[nodiscard]] std::uint8_t *host(std::uint32_t address, std::uint32_t length) const noexcept {
        const std::uint32_t c = address & 0x1FFFFFFFu;
        const std::uint32_t r = c - 0x08000000u;
        if (r < ram_size_ && length <= ram_size_ - r) return ram_ + r;
        const std::uint32_t v = c - 0x04000000u;
        if (v < 0x00800000u) {
            const std::uint32_t o = v & 0x001FFFFFu; // four mirrors of the 2 MiB EDRAM
            if (length <= 0x00200000u - o) return vram_ + o;
        }
        return nullptr;
    }
    [[nodiscard]] std::uint8_t rd8(std::uint32_t a) const noexcept {
        const std::uint8_t *p = host(a, 1);
        return p != nullptr ? *p : 0u;
    }
    [[nodiscard]] std::uint16_t rd16(std::uint32_t a) const noexcept {
        std::uint16_t v = 0;
        if (const std::uint8_t *p = host(a, 2)) std::memcpy(&v, p, 2);
        return v;
    }
    [[nodiscard]] std::uint32_t rd32(std::uint32_t a) const noexcept {
        std::uint32_t v = 0;
        if (const std::uint8_t *p = host(a, 4)) std::memcpy(&v, p, 4);
        return v;
    }
    void wr8(std::uint32_t a, std::uint8_t v) const noexcept {
        if (std::uint8_t *p = host(a, 1)) *p = v;
    }
    void wr16(std::uint32_t a, std::uint16_t v) const noexcept {
        if (std::uint8_t *p = host(a, 2)) std::memcpy(p, &v, 2);
    }
    void wr32(std::uint32_t a, std::uint32_t v) const noexcept {
        if (std::uint8_t *p = host(a, 4)) std::memcpy(p, &v, 4);
    }
    std::array<std::uint32_t, 256> regs_{};
    Matrix43 world_, view_, texgen_;
    std::array<Matrix43, 8> bones_{};
    Matrix44 proj_;
    unsigned world_i_{}, view_i_{}, proj_i_{}, texgen_i_{}, bone_i_{};
    std::array<float, 8> morph_{1, 0, 0, 0, 0, 0, 0, 0};
    std::uint32_t base_{}, offset_{};
    std::vector<std::uint32_t> call_stack_;
    std::array<std::uint32_t, 256> clut_{};
    Stats stats_;
};

// float24 → float (GE "float" parameters carry the top 24 bits of an IEEE single).
[[nodiscard]] float float24(std::uint32_t data);

} // namespace mhf::psp::gpu
