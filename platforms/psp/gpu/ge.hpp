#pragma once

// PSP Graphics Engine: display-list processor and software renderer (docs/05-runtime-hle.md §6).
//
// The software renderer is the reference implementation: it draws straight into guest VRAM, so every framebuffer
// effect a game relies on (render to texture, block transfers, reading back) behaves like the hardware. A Vulkan
// backend will later replace the drawing part behind the same state.

#include "psprecomp/guest_memory.hpp"

#include <array>
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
    };
    [[nodiscard]] const Stats &stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint32_t reg(std::uint32_t cmd) const noexcept { return regs_[cmd & 0xFF]; }

private:
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

    psprecomp::GuestMemory &mem_;
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
