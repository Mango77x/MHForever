#pragma once

// Hardware drawing behind the GE (docs/05-runtime-hle.md §6).
//
// The Ge class keeps doing everything the PSP does before rasterization (list processing, vertex decoding,
// transform, lighting, near-plane clipping, viewport mapping). With a backend installed, the resulting screen-space
// primitives go to the backend instead of the software rasterizer; the backend reads the rest of the draw state
// (textures, blending, tests, framebuffer) from the GE registers through the Ge it is given.

#include <cstdint>

namespace mhf::psp::gpu {

class Ge;
struct Vertex;

class Backend {
public:
    virtual ~Backend() = default;

    // Screen-space vertices: x, y in framebuffer pixels, z in 0..65535, w = 1/w for perspective correction (1 in
    // through mode), u, v normalized (through mode: texels), colors 0..255, fog 0..1.
    virtual void triangle(const Ge &ge, const Vertex &a, const Vertex &b, const Vertex &c) = 0;
    // Axis-aligned rectangle between two corners; color, depth and fog come from `b`.
    virtual void sprite(const Ge &ge, const Vertex &a, const Vertex &b) = 0;
    // A block transfer (GE copy between memory rectangles) has been performed in guest memory.
    virtual void block_transfer(std::uint32_t src, std::uint32_t src_stride, std::uint32_t sx, std::uint32_t sy,
                                std::uint32_t dst, std::uint32_t dst_stride, std::uint32_t dx, std::uint32_t dy,
                                std::uint32_t width, std::uint32_t height, std::uint32_t bytes_per_pixel) = 0;
    // The current display list has ended (or stalled): submit pending work.
    virtual void flush() = 0;
    // The game asked to show the framebuffer at `address` (VRAM, as given to sceDisplaySetFrameBuf).
    virtual void present(std::uint32_t address, std::uint32_t stride, std::uint32_t pixel_format) = 0;
};

} // namespace mhf::psp::gpu
