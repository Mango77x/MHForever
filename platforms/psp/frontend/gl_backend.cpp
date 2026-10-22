// OpenGL 3.3 drawing for the GE. See gl_backend.hpp and docs/05-runtime-hle.md §6.
//
// Model: every framebuffer address the game draws to gets a render target (color texture + depth buffer) at
// `scale` times the PSP resolution. Primitives arrive in screen space from the Ge; consecutive ones with the same
// draw state are batched into one draw call. Textures are decoded once and cached by content hash; a texture that
// lives inside a render target is sampled from that target directly (render-to-texture effects). Block transfers out
// of a render target are done on the GPU. The displayed framebuffer is drawn to the window, letterboxed, when the
// game flips; one the GE has not drawn (CPU-made screens, movies) is uploaded from guest memory instead.

#include "gl_backend.hpp"

#include "ge.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <optional>
#include <future>
#include <set>
#include <span>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

namespace mhf::psp {

namespace {

// GE registers used here (see platforms/psp/gpu/ge.cpp).
enum Reg : std::uint32_t {
    kVertexType = 0x12, kTextureMapEnable = 0x1E, kFogEnable = 0x1F, kAlphaBlendEnable = 0x21,
    kAlphaTestEnable = 0x22, kZTestEnable = 0x23, kStencilTestEnable = 0x24, kShadeMode = 0x50,
    kFramebufPtr = 0x9C, kFramebufWidth = 0x9D, kTexAddr0 = 0xA0, kTexBufWidth0 = 0xA8, kClutAddr = 0xB0,
    kClutAddrUpper = 0xB1, kTexSize0 = 0xB8, kTexMode = 0xC2, kTexFormat = 0xC3, kClutFormat = 0xC5,
    kTexFilter = 0xC6, kTexWrap = 0xC7, kTexFunc = 0xC9, kTexEnvColor = 0xCA, kFogColor = 0xCF,
    kFramebufPixFormat = 0xD2, kClearMode = 0xD3, kScissor1 = 0xD4, kScissor2 = 0xD5, kMinZ = 0xD6, kMaxZ = 0xD7,
    kAlphaTest = 0xDB, kZTest = 0xDE, kBlendMode = 0xDF, kBlendFixedA = 0xE0, kBlendFixedB = 0xE1,
    kZWriteDisable = 0xE7, kMaskRgb = 0xE8, kMaskAlpha = 0xE9, kViewportXScale = 0x42, kViewportYScale = 0x43,
    kViewportZScale = 0x44, kViewportXCenter = 0x45, kViewportYCenter = 0x46, kViewportZCenter = 0x47, kOffsetX = 0x4C,
    kOffsetY = 0x4D,
};

// Registers whose value changes how a primitive is drawn (anything else only affects vertex processing).
constexpr std::array<std::uint32_t, 36> kStateRegs = {
    kVertexType, kTextureMapEnable, kFogEnable, kAlphaBlendEnable, kAlphaTestEnable, kZTestEnable,
    kStencilTestEnable, kShadeMode, kFramebufPtr, kFramebufWidth, kTexAddr0, kTexBufWidth0, kClutAddr,
    kClutAddrUpper, kTexSize0, kTexMode, kTexFormat, kClutFormat, kTexFilter, kTexWrap, kTexFunc, kTexEnvColor,
    kFogColor, kFramebufPixFormat, kClearMode, kScissor1, kScissor2, kMinZ, kMaxZ, kAlphaTest, kZTest, kBlendMode,
    kBlendFixedA, kBlendFixedB, kZWriteDisable, kMaskRgb,
};

// OpenGL 1.x entry points (not every platform header declares pointer types for them).
#ifndef APIENTRY
#define APIENTRY
#endif
using MhfGetString = const GLubyte *(APIENTRY *)(GLenum);
using MhfGetError = GLenum(APIENTRY *)();
using MhfViewport = void(APIENTRY *)(GLint, GLint, GLsizei, GLsizei);
using MhfScissor = void(APIENTRY *)(GLint, GLint, GLsizei, GLsizei);
using MhfEnable = void(APIENTRY *)(GLenum);
using MhfDisable = void(APIENTRY *)(GLenum);
using MhfClear = void(APIENTRY *)(GLbitfield);
using MhfClearColor = void(APIENTRY *)(GLfloat, GLfloat, GLfloat, GLfloat);
using MhfColorMask = void(APIENTRY *)(GLboolean, GLboolean, GLboolean, GLboolean);
using MhfDepthMask = void(APIENTRY *)(GLboolean);
using MhfDepthFunc = void(APIENTRY *)(GLenum);
using MhfGenTextures = void(APIENTRY *)(GLsizei, GLuint *);
using MhfDeleteTextures = void(APIENTRY *)(GLsizei, const GLuint *);
using MhfBindTexture = void(APIENTRY *)(GLenum, GLuint);
using MhfTexImage2D = void(APIENTRY *)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
using MhfTexSubImage2D = void(APIENTRY *)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
using MhfTexParameteri = void(APIENTRY *)(GLenum, GLenum, GLint);
using MhfPixelStorei = void(APIENTRY *)(GLenum, GLint);
using MhfDrawArrays = void(APIENTRY *)(GLenum, GLint, GLsizei);
using MhfReadPixels = void(APIENTRY *)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);

// ---------------------------------------------------------------- OpenGL entry points

#define MHF_GL_FUNCTIONS(X)                                                                                        \
    X(MhfGetString, GetString, glGetString)                                                                  \
    X(MhfGetError, GetError, glGetError)                                                                     \
    X(MhfViewport, Viewport, glViewport)                                                                     \
    X(MhfScissor, Scissor, glScissor)                                                                        \
    X(MhfEnable, Enable, glEnable)                                                                           \
    X(MhfDisable, Disable, glDisable)                                                                        \
    X(MhfClear, Clear, glClear)                                                                              \
    X(MhfClearColor, ClearColor, glClearColor)                                                               \
    X(MhfColorMask, ColorMask, glColorMask)                                                                  \
    X(MhfDepthMask, DepthMask, glDepthMask)                                                                  \
    X(MhfDepthFunc, DepthFunc, glDepthFunc)                                                                  \
    X(PFNGLBLENDFUNCSEPARATEPROC, BlendFuncSeparate, glBlendFuncSeparate)                                          \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, BlendEquationSeparate, glBlendEquationSeparate)                              \
    X(PFNGLBLENDCOLORPROC, BlendColor, glBlendColor)                                                               \
    X(MhfGenTextures, GenTextures, glGenTextures)                                                            \
    X(MhfDeleteTextures, DeleteTextures, glDeleteTextures)                                                   \
    X(MhfBindTexture, BindTexture, glBindTexture)                                                            \
    X(MhfTexImage2D, TexImage2D, glTexImage2D)                                                               \
    X(MhfTexSubImage2D, TexSubImage2D, glTexSubImage2D)                                                      \
    X(MhfTexParameteri, TexParameteri, glTexParameteri)                                                      \
    X(PFNGLACTIVETEXTUREPROC, ActiveTexture, glActiveTexture)                                                      \
    X(MhfPixelStorei, PixelStorei, glPixelStorei)                                                            \
    X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers, glGenFramebuffers)                                                \
    X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers, glDeleteFramebuffers)                                       \
    X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer, glBindFramebuffer)                                                \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D, glFramebufferTexture2D)                                 \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, FramebufferRenderbuffer, glFramebufferRenderbuffer)                        \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus, glCheckFramebufferStatus)                           \
    X(PFNGLGENRENDERBUFFERSPROC, GenRenderbuffers, glGenRenderbuffers)                                             \
    X(PFNGLDELETERENDERBUFFERSPROC, DeleteRenderbuffers, glDeleteRenderbuffers)                                    \
    X(PFNGLBINDRENDERBUFFERPROC, BindRenderbuffer, glBindRenderbuffer)                                             \
    X(PFNGLRENDERBUFFERSTORAGEPROC, RenderbufferStorage, glRenderbufferStorage)                                    \
    X(PFNGLBLITFRAMEBUFFERPROC, BlitFramebuffer, glBlitFramebuffer)                                                \
    X(PFNGLCREATESHADERPROC, CreateShader, glCreateShader)                                                         \
    X(PFNGLSHADERSOURCEPROC, ShaderSource, glShaderSource)                                                         \
    X(PFNGLCOMPILESHADERPROC, CompileShader, glCompileShader)                                                      \
    X(PFNGLGETSHADERIVPROC, GetShaderiv, glGetShaderiv)                                                            \
    X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog, glGetShaderInfoLog)                                             \
    X(PFNGLCREATEPROGRAMPROC, CreateProgram, glCreateProgram)                                                      \
    X(PFNGLATTACHSHADERPROC, AttachShader, glAttachShader)                                                         \
    X(PFNGLLINKPROGRAMPROC, LinkProgram, glLinkProgram)                                                            \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv, glGetProgramiv)                                                         \
    X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog, glGetProgramInfoLog)                                          \
    X(PFNGLUSEPROGRAMPROC, UseProgram, glUseProgram)                                                               \
    X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation, glGetUniformLocation)                                       \
    X(PFNGLUNIFORM1IPROC, Uniform1i, glUniform1i)                                                                  \
    X(PFNGLUNIFORM1FPROC, Uniform1f, glUniform1f)                                                                  \
    X(PFNGLUNIFORM2FPROC, Uniform2f, glUniform2f)                                                                  \
    X(PFNGLUNIFORM3FPROC, Uniform3f, glUniform3f)                                                                  \
    X(PFNGLUNIFORM4FPROC, Uniform4f, glUniform4f)                                                                  \
    X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays, glGenVertexArrays)                                                \
    X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray, glBindVertexArray)                                                \
    X(PFNGLGENBUFFERSPROC, GenBuffers, glGenBuffers)                                                               \
    X(PFNGLBINDBUFFERPROC, BindBuffer, glBindBuffer)                                                               \
    X(PFNGLBUFFERDATAPROC, BufferData, glBufferData)                                                               \
    X(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer, glVertexAttribPointer)                                    \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray, glEnableVertexAttribArray)                        \
    X(MhfDrawArrays, DrawArrays, glDrawArrays)                                                                     \
    X(MhfReadPixels, ReadPixels, glReadPixels)

struct Gl {
#define MHF_GL_MEMBER(type, name, symbol) type name{};
    MHF_GL_FUNCTIONS(MHF_GL_MEMBER)
#undef MHF_GL_MEMBER
    bool load() {
        bool ok = true;
#define MHF_GL_LOAD(type, name, symbol)                                                                            \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#symbol));                                                 \
    if (name == nullptr) {                                                                                         \
        std::fprintf(stderr, "[gl] missing %s\n", #symbol);                                                       \
        ok = false;                                                                                                \
    }
        MHF_GL_FUNCTIONS(MHF_GL_LOAD)
#undef MHF_GL_LOAD
        return ok;
    }
};

// ---------------------------------------------------------------- shaders

constexpr const char *kDrawVertex = R"(#version 330 core
layout(location = 0) in vec4 a_pos;   // x, y in PSP pixels, z 0..65535, 1/w
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec4 a_spec;
layout(location = 4) in float a_fog;
uniform vec2 u_target;                // render target size in PSP pixels
uniform vec4 u_uv_xform;              // scale.xy, offset.zw
out vec2 v_uv;
out vec4 v_color;
out vec3 v_spec;
out float v_fog;
void main() {
    float w = 1.0 / a_pos.w;
    vec2 ndc = a_pos.xy / u_target * 2.0 - 1.0;
    float z = clamp(a_pos.z / 65535.0, 0.0, 1.0) * 2.0 - 1.0;
    gl_Position = vec4(ndc * w, z * w, w);
    v_uv = a_uv * u_uv_xform.xy + u_uv_xform.zw;
    v_color = a_color;
    v_spec = a_spec.rgb;
    v_fog = a_fog;
}
)";

constexpr const char *kDrawFragment = R"(#version 330 core
in vec2 v_uv;
in vec4 v_color;
in vec3 v_spec;
in float v_fog;
uniform sampler2D u_tex;
uniform int u_tex_enable;
uniform int u_tex_func;
uniform int u_tex_rgba;
uniform int u_tex_double;
uniform vec3 u_env;
uniform int u_clear;
uniform int u_fog_enable;
uniform vec3 u_fog_color;
uniform int u_alpha_func;
uniform int u_alpha_ref;
uniform int u_alpha_mask;
uniform int u_zrange_enable;
uniform vec2 u_zrange;
out vec4 o_color;
bool compare(int f, int a, int b) {
    if (f == 0) return false;
    if (f == 1) return true;
    if (f == 2) return a == b;
    if (f == 3) return a != b;
    if (f == 4) return a < b;
    if (f == 5) return a <= b;
    if (f == 6) return a > b;
    return a >= b;
}
void main() {
    vec4 c = v_color;
    if (u_clear == 0) {
        if (u_tex_enable != 0) {
            vec4 t = texture(u_tex, v_uv);
            if (u_tex_func == 0) { c.rgb *= t.rgb; if (u_tex_rgba != 0) c.a *= t.a; }
            else if (u_tex_func == 1) { c.rgb = u_tex_rgba != 0 ? mix(c.rgb, t.rgb, t.a) : t.rgb; }
            else if (u_tex_func == 2) { c.rgb = mix(c.rgb, u_env, t.rgb); if (u_tex_rgba != 0) c.a *= t.a; }
            else if (u_tex_func == 3) { c.rgb = t.rgb; if (u_tex_rgba != 0) c.a = t.a; }
            else { c.rgb += t.rgb; if (u_tex_rgba != 0) c.a *= t.a; }
            if (u_tex_double != 0) c.rgb *= 2.0;
        }
        c.rgb = min(c.rgb + v_spec, vec3(1.0));
        if (u_fog_enable != 0) c.rgb = mix(u_fog_color, c.rgb, clamp(v_fog, 0.0, 1.0));
        if (u_alpha_func != 1) {
            int a = int(clamp(c.a, 0.0, 1.0) * 255.0 + 0.5) & u_alpha_mask;
            if (!compare(u_alpha_func, a, u_alpha_ref & u_alpha_mask)) discard;
        }
        if (u_zrange_enable != 0) {
            float z = gl_FragCoord.z * 65535.0;
            if (z < u_zrange.x || z > u_zrange.y + 0.5) discard;
        }
    }
    o_color = clamp(c, 0.0, 1.0);
}
)";

// Draws a texture over the whole viewport; row 0 of the texture (PSP line 0) at the top.
constexpr const char *kPresentVertex = R"(#version 330 core
uniform vec2 u_uv_max;
out vec2 v_uv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv = vec2(p.x, 1.0 - p.y) * u_uv_max;
}
)";

constexpr const char *kPresentFragment = R"(#version 330 core
in vec2 v_uv;
uniform sampler2D u_tex;
out vec4 o_color;
void main() { o_color = vec4(texture(u_tex, v_uv).rgb, 1.0); }
)";

// The dialog overlay: premultiplied colour, faded in by u_alpha.
constexpr const char *kOverlayFragment = R"(#version 330 core
in vec2 v_uv;
uniform sampler2D u_tex;
uniform float u_alpha;
out vec4 o_color;
void main() { o_color = texture(u_tex, v_uv) * u_alpha; }
)";

// ---------------------------------------------------------------- pixel helpers

std::uint32_t expand(std::uint32_t v, int bits) {
    return bits == 5 ? (v << 3u) | (v >> 2u) : bits == 6 ? (v << 2u) | (v >> 4u) : (v << 4u) | v;
}

// PSP framebuffer pixel (format 0..3) to ABGR8888.
std::uint32_t framebuffer_pixel(const std::uint8_t *p, std::uint32_t format) {
    if (format == 3) {
        std::uint32_t c;
        std::memcpy(&c, p, 4);
        return c;
    }
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    std::uint32_t r, g, b, a;
    if (format == 0) { r = expand(v & 31u, 5); g = expand((v >> 5u) & 63u, 6); b = expand((v >> 11u) & 31u, 5); a = 255; }
    else if (format == 1) { r = expand(v & 31u, 5); g = expand((v >> 5u) & 31u, 5); b = expand((v >> 10u) & 31u, 5); a = (v & 0x8000u) ? 255u : 0u; }
    else { r = expand(v & 15u, 4); g = expand((v >> 4u) & 15u, 4); b = expand((v >> 8u) & 15u, 4); a = expand(v >> 12u, 4); }
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

void unpack_rgb(std::uint32_t c, float out[3]) {
    out[0] = static_cast<float>(c & 0xFFu) / 255.0f;
    out[1] = static_cast<float>((c >> 8u) & 0xFFu) / 255.0f;
    out[2] = static_cast<float>((c >> 16u) & 0xFFu) / 255.0f;
}

struct GpuVertex {
    float x, y, z, inv_w;
    float u, v;
    std::uint8_t color[4];
    std::uint8_t spec[4];
    float fog;
};

using DrawKey = std::array<std::uint32_t, kStateRegs.size() + 2>;

// The value of a state register as it was when `key` was captured (kStateRegs plus the alpha mask).
std::uint32_t key_reg(const DrawKey &key, std::uint32_t reg) {
    if (reg == kMaskAlpha) return key[kStateRegs.size() + 1];
    for (std::size_t i = 0; i < kStateRegs.size(); ++i) {
        if (kStateRegs[i] == reg) return key[i];
    }
    return 0; // not a draw-state register: nothing here reads one
}

// Everything needed to set up OpenGL for a batch of primitives: the registers plus what was resolved from guest
// memory when the batch started (the texture and the render target).
struct DrawSetup {
    DrawKey key{};
    GLuint texture{};     // 0: untextured
    float uv_xform[4]{1, 1, 0, 0};
    bool through{};
    float tex_width{1}, tex_height{1};
    std::uint32_t target{}; // VRAM offset of the render target
    bool self_sampled{};    // the texture is a copy of the target being drawn: it cannot be replayed later
};

// The camera a vertex was placed with: view and projection matrices, viewport and screen offset.
struct Camera {
    std::array<float, 12> view{};
    std::array<float, 16> proj{};
    std::array<float, 8> viewport{}; // x, y, z scale; x, y, z center; x, y offset
    bool operator==(const Camera &) const = default;
};
constexpr std::uint16_t kNoCamera = 0xFFFF;

// Where a recorded vertex came from, to place it again for an in-between picture (frame interpolation): its point in
// the world (before the view matrix) and the camera. Sprite corners take x and y from different points.
struct Origin {
    float x_world[3]{}, y_world[3]{};
    std::uint64_t x_key{}, y_key{};  // the same point in another flip has the same key (0: no identity)
    std::uint16_t camera{kNoCamera}; // kNoCamera: drawn where it was recorded
    bool sprite{};
    // Where the same points were in the previous flip, when they were drawn then (filled in for the flip shown).
    bool x_moved{}, y_moved{};
    float x_before[3]{}, y_before[3]{};
};

// One batch of primitives as it was drawn, kept to draw it again between two flips (frame interpolation).
struct TapeEntry {
    DrawSetup setup;
    std::vector<GpuVertex> vertices;
    std::vector<Origin> origins; // one per vertex
};

// A picture waiting for its moment on screen: `phase` 1 is the flip itself, below that a picture between the
// previous flip (0) and this one.
struct Picture {
    float phase{1};
    std::chrono::steady_clock::time_point due;
};

// A copy of a finished picture (the last two are kept).
struct Slot {
    GLuint texture{}, fbo{};
    GLsizei width{}, height{};
};

std::uint8_t to_byte(float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

// ---------------------------------------------------------------- the backend

class GlBackend final : public gpu::Backend {
public:
    GlBackend(SDL_Window *window, int scale, psprecomp::GuestMemory &memory, bool smooth_textures,
              OverlayImage *overlay, int factor)
        : window_(window), scale_(std::max(1, scale)), memory_(memory), smooth_textures_(smooth_textures),
          overlay_(overlay), factor_(std::clamp(factor, 1, 6)), interpolate_(factor > 1) {}

    ~GlBackend() override {
        if (context_ != nullptr) SDL_GL_DestroyContext(context_);
    }

    bool init();

    void triangle(const gpu::Ge &ge, const gpu::Vertex &a, const gpu::Vertex &b, const gpu::Vertex &c) override {
        prepare(ge);
        const bool flat = (ge.reg(kShadeMode) & 1u) == 0;
        push(a, flat ? &c : nullptr);
        push(b, flat ? &c : nullptr);
        push(c, nullptr);
        if (interpolate_) {
            const std::uint16_t camera = a.has_world && b.has_world && c.has_world ? camera_for(ge) : kNoCamera;
            record(a, a, camera, false);
            record(b, b, camera, false);
            record(c, c, camera, false);
        }
    }

    void draw_started(std::uint32_t vertex_address) override {
        if (interpolate_) draw_use_ = ++draw_uses_[vertex_address];
    }

    void sprite(const gpu::Ge &ge, const gpu::Vertex &a, const gpu::Vertex &b) override {
        prepare(ge);
        gpu::Vertex tl = b, tr = b, bl = b, br = b;
        tl.x = a.x; tl.y = a.y; tl.u = a.u; tl.v = a.v;
        tr.x = b.x; tr.y = a.y; tr.u = b.u; tr.v = a.v;
        bl.x = a.x; bl.y = b.y; bl.u = a.u; bl.v = b.v;
        for (gpu::Vertex *v : {&tl, &tr, &bl, &br}) v->w = 1.0f; // sprites interpolate linearly
        push(tl, nullptr);
        push(tr, nullptr);
        push(bl, nullptr);
        push(tr, nullptr);
        push(br, nullptr);
        push(bl, nullptr);
        if (interpolate_) {
            const std::uint16_t camera = a.has_world && b.has_world ? camera_for(ge) : kNoCamera;
            record(a, a, camera, true); // top left: x and y from a
            record(b, a, camera, true); // top right
            record(a, b, camera, true); // bottom left
            record(b, a, camera, true);
            record(b, b, camera, true); // bottom right
            record(a, b, camera, true);
        }
    }

    void block_transfer(std::uint32_t src, std::uint32_t src_stride, std::uint32_t sx, std::uint32_t sy,
                        std::uint32_t dst, std::uint32_t dst_stride, std::uint32_t dx, std::uint32_t dy,
                        std::uint32_t width, std::uint32_t height, std::uint32_t bpp) override;

    void flush() override;

    void present(std::uint32_t address, std::uint32_t stride, std::uint32_t pixel_format) override;

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_wakeup() const override {
        if (!pictures_.empty()) return pictures_.front().due;
        return std::nullopt;
    }
    void tick() override;

private:
    struct RenderTarget {
        std::uint32_t address{}; // VRAM offset
        std::uint32_t stride{};  // pixels per line
        std::uint32_t width{}, height{};
        std::uint32_t format{};
        GLuint fbo{}, color{}, depth{};
        std::uint64_t last_draw{}; // present count when last drawn by the GE
        std::uint64_t last_use{};
        [[nodiscard]] std::uint32_t bytes_per_pixel() const { return format == 3 ? 4u : 2u; }
        [[nodiscard]] std::uint32_t end() const { return address + stride * height * bytes_per_pixel(); }
    };
    struct CachedTexture {
        GLuint texture{};
        std::uint64_t hash{};
        std::uint64_t checked_frame{~0ull};
        std::uint64_t checked_clut{~0ull};
        std::uint64_t last_use{};
    };

    void prepare(const gpu::Ge &ge) {
        DrawKey key{};
        for (std::size_t i = 0; i < kStateRegs.size(); ++i) key[i] = ge.reg(kStateRegs[i]);
        key[kStateRegs.size()] = static_cast<std::uint32_t>(ge.clut_generation());
        key[kStateRegs.size() + 1] = ge.reg(kMaskAlpha);
        if (have_state_ && key == state_key_) return;
        draw_batch();
        state_key_ = key;
        have_state_ = true;
        apply_state(ge);
    }

    void push(const gpu::Vertex &v, const gpu::Vertex *color_from) {
        const gpu::Vertex &c = color_from != nullptr ? *color_from : v;
        GpuVertex g{};
        g.x = v.x;
        g.y = v.y;
        g.z = v.z;
        g.inv_w = through_ ? 1.0f : (v.w > 0.0f ? v.w : 1.0f);
        g.u = through_ ? v.u / tex_width_ : v.u;
        g.v = through_ ? v.v / tex_height_ : v.v;
        g.color[0] = to_byte(c.r); g.color[1] = to_byte(c.g); g.color[2] = to_byte(c.b); g.color[3] = to_byte(c.a);
        g.spec[0] = to_byte(c.sr); g.spec[1] = to_byte(c.sg); g.spec[2] = to_byte(c.sb); g.spec[3] = 0;
        g.fog = v.fog;
        batch_.push_back(g);
    }

    // Frame interpolation: the camera of the vertices being drawn (an index into cameras_), and where each recorded
    // vertex came from.
    std::uint16_t camera_for(const gpu::Ge &ge) {
        if (camera_ != kNoCamera && camera_generation_ == ge.camera_generation()) return camera_; // nothing was written
        camera_generation_ = ge.camera_generation();
        return find_camera(ge);
    }
    std::uint16_t find_camera(const gpu::Ge &ge) {
        Camera c;
        std::copy(ge.view_matrix().m.begin(), ge.view_matrix().m.end(), c.view.begin());
        std::copy(ge.proj_matrix().m.begin(), ge.proj_matrix().m.end(), c.proj.begin());
        c.viewport = {gpu::float24(ge.reg(kViewportXScale)), gpu::float24(ge.reg(kViewportYScale)),
                      gpu::float24(ge.reg(kViewportZScale)), gpu::float24(ge.reg(kViewportXCenter)),
                      gpu::float24(ge.reg(kViewportYCenter)), gpu::float24(ge.reg(kViewportZCenter)),
                      static_cast<float>(ge.reg(kOffsetX) & 0xFFFFu) / 16.0f,
                      static_cast<float>(ge.reg(kOffsetY) & 0xFFFFu) / 16.0f};
        if (camera_ < cameras_.size() && cameras_[camera_] == c) return camera_;
        for (std::size_t i = 0; i < cameras_.size(); ++i) {
            if (cameras_[i] == c) return camera_ = static_cast<std::uint16_t>(i);
        }
        if (cameras_.size() >= kNoCamera) return kNoCamera;
        cameras_.push_back(c);
        return camera_ = static_cast<std::uint16_t>(cameras_.size() - 1);
    }
    [[nodiscard]] std::uint64_t key_of(const gpu::Vertex &v) const {
        return v.source == 0 ? 0 : (static_cast<std::uint64_t>(draw_use_) << 32u) | v.source;
    }
    void record(const gpu::Vertex &x_from, const gpu::Vertex &y_from, std::uint16_t camera, bool sprite) {
        Origin o;
        std::copy(x_from.world, x_from.world + 3, o.x_world);
        std::copy(y_from.world, y_from.world + 3, o.y_world);
        o.x_key = key_of(x_from);
        o.y_key = key_of(y_from);
        o.camera = camera;
        o.sprite = sprite;
        batch_origins_.push_back(o);
    }
    void place_again(const TapeEntry &entry, const std::vector<Camera> &cameras, std::span<GpuVertex> vertices,
                     float t) const;

    void apply_state(const gpu::Ge &ge);
    void emit_state(const DrawSetup &setup, RenderTarget &rt);
    void draw_vertices(const std::vector<GpuVertex> &vertices);
    void show(GLuint texture, float uv_max_x, float uv_max_y);
    void finish_flip();
    bool present_interpolated(RenderTarget &shown, const float uv_max[2]);
    void show_picture(const Picture &picture);
    void flush_pictures();
    void ensure_slot(Slot &slot, GLsizei w, GLsizei h);
    void render_between(const RenderTarget &shown, const std::vector<TapeEntry> &now,
                        const std::vector<TapeEntry> &before, const Slot &before_slot, float t);
    void draw_batch();
    void read_back();
    RenderTarget &target_for(std::uint32_t address, std::uint32_t stride, std::uint32_t format,
                             std::uint32_t needed_height);
    RenderTarget *find_target(std::uint32_t vram_offset, bool drawn_only);
    void allocate(RenderTarget &rt, std::uint32_t height);
    GLuint texture_for(const gpu::Ge &ge, float uv_xform[4]);
    void upload_rect(RenderTarget &rt, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h);
    GLuint compile(const char *vertex, const char *fragment);
    GLint uniform(GLuint program, const char *name) { return gl_.GetUniformLocation(program, name); }
    void set_texture_params(bool linear, bool clamp_s, bool clamp_t);
    // Debugging: MHF_GL_DUMP=<folder> writes every MHF_GL_DUMP_EVERY-th (default 60) presented frame as a PPM image,
    // starting at presented frame MHF_GL_DUMP_FROM (default 0).
    void dump_window(int w, int h);
    void draw_overlay();

    SDL_Window *window_;
    SDL_GLContext context_{};
    int scale_;
    psprecomp::GuestMemory &memory_;
    bool smooth_textures_ = false;
    OverlayImage *overlay_ = nullptr;
    GLuint overlay_program_{}, overlay_texture_{};
    GLint overlay_tex_{}, overlay_alpha_{}, overlay_uv_max_{};
    std::uint64_t overlay_generation_{~0ull};
    bool overlay_visible_{};
    std::uint64_t overlay_shown_at_{}; // SDL ticks (ms) when it appeared, for the fade-in
    Gl gl_;

    GLuint draw_program_{}, present_program_{}, vao_{}, vbo_{}, empty_vao_{};
    struct {
        GLint target, uv_xform, tex_enable, tex_func, tex_rgba, tex_double, env, clear, fog_enable, fog_color,
            alpha_func, alpha_ref, alpha_mask, zrange_enable, zrange, tex;
    } u_{};
    GLint present_uv_max_{}, present_tex_{};

    std::map<std::uint32_t, RenderTarget> targets_; // by VRAM offset
    RenderTarget *current_{};
    std::unordered_map<std::uint64_t, CachedTexture> textures_;
    // Textures replaced by new content while a recorded batch (frame interpolation) may still draw with them; they
    // are deleted a few flips later.
    std::vector<std::pair<std::uint64_t, GLuint>> retired_textures_;
    GLuint copy_texture_{}, copy_fbo_{};               // for sampling the target being drawn
    std::uint32_t copy_width_{}, copy_height_{};
    GLuint upload_texture_{};                          // CPU pixels on their way to a target or the screen
    GLuint display_texture_{};
    std::vector<std::uint32_t> pixels_;

    DrawKey state_key_{};
    DrawSetup setup_{}; // of the batch being collected

    // Frame interpolation (enhancement, off by default). The game flips at 30 fps and the output runs at
    // `factor_` times that: every flip is followed by factor_ - 1 pictures between it and the next one, each made by
    // drawing the flip's batches again through a camera between the two flips (3D), with things that moved on their
    // own placed between their two positions. Pictures are shown at even intervals on a timer the GE worker honours
    // only while it has no display list to run, so they never slow the game down (late ones are dropped); a flip is
    // shown (factor_ - 1)/factor_ of a frame late.
    int factor_ = 1;
    bool interpolate_ = false;
    std::vector<TapeEntry> tape_;           // batches since the last flip
    std::vector<TapeEntry> previous_;       // the last flip's batches that drew to the picture it showed
    std::vector<Camera> cameras_;           // cameras used since the last flip (Origin::camera indexes them)
    std::vector<Camera> previous_cameras_;  // the last flip's
    std::uint16_t camera_{kNoCamera};       // the last one found
    std::uint64_t camera_generation_{};     // Ge::camera_generation() when it was found
    std::unordered_map<std::uint32_t, std::uint32_t> draw_uses_; // drawing commands per vertex address since the flip
    std::uint32_t draw_use_{};              // of the current drawing command: tells repeated draws of a model apart
    struct PointSlot {
        std::uint64_t key{};
        const float *position{};
    };
    std::vector<PointSlot> point_table_; // scratch: the previous flip's points by key (open addressing, reused)
    std::set<std::uint32_t> touched_;       // render targets written by something other than batches (copies)
    bool previous_valid_ = false;
    Slot slots_[2];
    int fill_slot_ = 0;
    RenderTarget scratch_;                  // where in-between pictures are drawn
    // The flip being shown: what later pictures are made from.
    struct {
        const std::vector<TapeEntry> *now{}; // this flip's batches: previous_ once the flip has been recorded
        std::vector<TapeEntry> before;
        std::vector<Camera> cameras;        // of `now`
        std::vector<Camera> cameras_before; // the camera of `before` each one continues (or the same one)
        std::vector<int> partner;           // per batch of `now`: the 2D batch of `before` it continues, or -1
        std::uint32_t address{}, width{}, height{}, stride{}, format{};
        int before_slot{};
        GLuint final_texture{};
        float uv_max[2]{1, 1};
        std::chrono::steady_clock::duration slack{};
    } flip_;
    std::deque<Picture> pictures_;
    // In-between pictures are built in one buffer, uploaded once, and drawn batch by batch from ranges of it.
    std::vector<GpuVertex> staging_;
    struct Range {
        std::size_t first{}, count{};
    };
    std::vector<Range> ranges_;
    // What an in-between picture costs the drawing thread (MHF_INTERP_LOG=1): calculating the vertices, uploading them,
    // setting the state of each batch, and the draw calls.
    struct PictureCost {
        std::uint64_t pictures{}, batches{}, vertices{};
        double calc_us{}, upload_us{}, state_us{}, draw_us{};
    } cost_;
    std::chrono::steady_clock::time_point last_flip_{};
    std::uint64_t shown_{1};                // pictures put on screen
    struct {
        std::uint64_t interpolated{}, skipped_shape{}, skipped_copy{}, skipped_timing{}, skipped_first{}, skipped_ui{}, late{};
    } stats_;
    bool have_state_{};
    bool through_{};
    float tex_width_{1}, tex_height_{1};
    std::vector<GpuVertex> batch_;
    std::vector<Origin> batch_origins_; // frame interpolation: one per vertex of batch_
    std::uint64_t frame_{1};
};

bool GlBackend::init() {
    context_ = SDL_GL_CreateContext(window_);
    if (context_ == nullptr) {
        std::fprintf(stderr, "[gl] cannot create an OpenGL 3.3 context: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window_, context_);
    SDL_GL_SetSwapInterval(0); // pacing comes from the emulated vblank
    if (!gl_.load()) return false;
    std::fprintf(stderr, "[gl] %s, %s, internal resolution %dx%d\n",
                 reinterpret_cast<const char *>(gl_.GetString(GL_RENDERER)),
                 reinterpret_cast<const char *>(gl_.GetString(GL_VERSION)), 480 * scale_, 272 * scale_);
    draw_program_ = compile(kDrawVertex, kDrawFragment);
    present_program_ = compile(kPresentVertex, kPresentFragment);
    overlay_program_ = compile(kPresentVertex, kOverlayFragment);
    if (draw_program_ == 0 || present_program_ == 0 || overlay_program_ == 0) return false;
    overlay_tex_ = uniform(overlay_program_, "u_tex");
    overlay_alpha_ = uniform(overlay_program_, "u_alpha");
    overlay_uv_max_ = uniform(overlay_program_, "u_uv_max");
    gl_.GenTextures(1, &overlay_texture_);
    u_.target = uniform(draw_program_, "u_target");
    u_.uv_xform = uniform(draw_program_, "u_uv_xform");
    u_.tex_enable = uniform(draw_program_, "u_tex_enable");
    u_.tex_func = uniform(draw_program_, "u_tex_func");
    u_.tex_rgba = uniform(draw_program_, "u_tex_rgba");
    u_.tex_double = uniform(draw_program_, "u_tex_double");
    u_.env = uniform(draw_program_, "u_env");
    u_.clear = uniform(draw_program_, "u_clear");
    u_.fog_enable = uniform(draw_program_, "u_fog_enable");
    u_.fog_color = uniform(draw_program_, "u_fog_color");
    u_.alpha_func = uniform(draw_program_, "u_alpha_func");
    u_.alpha_ref = uniform(draw_program_, "u_alpha_ref");
    u_.alpha_mask = uniform(draw_program_, "u_alpha_mask");
    u_.zrange_enable = uniform(draw_program_, "u_zrange_enable");
    u_.zrange = uniform(draw_program_, "u_zrange");
    u_.tex = uniform(draw_program_, "u_tex");
    present_uv_max_ = uniform(present_program_, "u_uv_max");
    present_tex_ = uniform(present_program_, "u_tex");

    gl_.GenVertexArrays(1, &vao_);
    gl_.GenVertexArrays(1, &empty_vao_);
    gl_.GenBuffers(1, &vbo_);
    gl_.BindVertexArray(vao_);
    gl_.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    const auto stride = static_cast<GLsizei>(sizeof(GpuVertex));
    gl_.VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(GpuVertex, x)));
    gl_.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(GpuVertex, u)));
    gl_.VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void *>(offsetof(GpuVertex, color)));
    gl_.VertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void *>(offsetof(GpuVertex, spec)));
    gl_.VertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(GpuVertex, fog)));
    for (GLuint i = 0; i < 5; ++i) gl_.EnableVertexAttribArray(i);

    GLuint textures[4];
    gl_.GenTextures(4, textures);
    copy_texture_ = textures[0];
    upload_texture_ = textures[1];
    display_texture_ = textures[2];
    gl_.GenFramebuffers(1, &copy_fbo_);
    gl_.Enable(GL_DEPTH_CLAMP);
    gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl_.UseProgram(draw_program_);
    gl_.Uniform1i(u_.tex, 0);
    batch_.reserve(65536);
    return true;
}

GLuint GlBackend::compile(const char *vertex, const char *fragment) {
    auto stage = [&](GLenum type, const char *source) -> GLuint {
        const GLuint s = gl_.CreateShader(type);
        gl_.ShaderSource(s, 1, &source, nullptr);
        gl_.CompileShader(s);
        GLint ok = 0;
        gl_.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048];
            gl_.GetShaderInfoLog(s, sizeof log, nullptr, log);
            std::fprintf(stderr, "[gl] shader error: %s\n", log);
            return 0;
        }
        return s;
    };
    const GLuint vs = stage(GL_VERTEX_SHADER, vertex), fs = stage(GL_FRAGMENT_SHADER, fragment);
    if (vs == 0 || fs == 0) return 0;
    const GLuint p = gl_.CreateProgram();
    gl_.AttachShader(p, vs);
    gl_.AttachShader(p, fs);
    gl_.LinkProgram(p);
    GLint ok = 0;
    gl_.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        gl_.GetProgramInfoLog(p, sizeof log, nullptr, log);
        std::fprintf(stderr, "[gl] link error: %s\n", log);
        return 0;
    }
    return p;
}

// ---------------------------------------------------------------- render targets

void GlBackend::allocate(RenderTarget &rt, std::uint32_t height) {
    const GLsizei w = static_cast<GLsizei>(rt.width) * scale_, h = static_cast<GLsizei>(height) * scale_;
    GLuint color = 0, depth = 0, fbo = 0;
    gl_.GenTextures(1, &color);
    gl_.BindTexture(GL_TEXTURE_2D, color);
    gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    set_texture_params(true, true, true);
    gl_.GenRenderbuffers(1, &depth);
    gl_.BindRenderbuffer(GL_RENDERBUFFER, depth);
    gl_.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    gl_.GenFramebuffers(1, &fbo);
    gl_.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    gl_.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    gl_.Disable(GL_SCISSOR_TEST);
    gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl_.DepthMask(GL_TRUE);
    gl_.ClearColor(0, 0, 0, 0);
    gl_.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (rt.fbo != 0) {
        // Keep what was drawn so far when a target grows.
        gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, rt.fbo);
        gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        const GLsizei old_h = static_cast<GLsizei>(rt.height) * scale_;
        gl_.BlitFramebuffer(0, 0, w, old_h, 0, 0, w, old_h, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        gl_.DeleteFramebuffers(1, &rt.fbo);
        gl_.DeleteTextures(1, &rt.color);
        gl_.DeleteRenderbuffers(1, &rt.depth);
    }
    rt.fbo = fbo;
    rt.color = color;
    rt.depth = depth;
    rt.height = height;
}

GlBackend::RenderTarget &GlBackend::target_for(std::uint32_t address, std::uint32_t stride, std::uint32_t format,
                                              std::uint32_t needed_height) {
    stride = std::clamp<std::uint32_t>(stride, 1u, 1024u);
    needed_height = std::clamp<std::uint32_t>(needed_height, 1u, 1024u);
    auto it = targets_.find(address);
    if (it != targets_.end() && (it->second.stride != stride || it->second.format != format)) {
        auto &old = it->second;
        gl_.DeleteFramebuffers(1, &old.fbo);
        gl_.DeleteTextures(1, &old.color);
        gl_.DeleteRenderbuffers(1, &old.depth);
        targets_.erase(it);
        it = targets_.end();
    }
    if (it == targets_.end()) {
        RenderTarget rt;
        rt.address = address;
        rt.stride = stride;
        rt.width = stride;
        rt.format = format;
        allocate(rt, std::max(needed_height, stride >= 480u ? 272u : std::min(stride, 512u)));
        it = targets_.emplace(address, rt).first;
        // Bound the number of live targets: drop the least recently used ones.
        while (targets_.size() > 48) {
            auto victim = targets_.end();
            for (auto i = targets_.begin(); i != targets_.end(); ++i) {
                if (i != it && (victim == targets_.end() || i->second.last_use < victim->second.last_use)) victim = i;
            }
            if (victim == targets_.end()) break;
            gl_.DeleteFramebuffers(1, &victim->second.fbo);
            gl_.DeleteTextures(1, &victim->second.color);
            gl_.DeleteRenderbuffers(1, &victim->second.depth);
            targets_.erase(victim);
        }
    } else if (needed_height > it->second.height) {
        allocate(it->second, needed_height);
    }
    it->second.last_use = frame_;
    return it->second;
}

GlBackend::RenderTarget *GlBackend::find_target(std::uint32_t vram_offset, bool drawn_only) {
    RenderTarget *best = nullptr;
    for (auto &[address, rt] : targets_) {
        if (vram_offset >= rt.address && vram_offset < rt.end() && (!drawn_only || rt.last_draw != 0)) {
            if (best == nullptr || rt.last_draw > best->last_draw) best = &rt;
        }
    }
    return best;
}

// ---------------------------------------------------------------- state

void GlBackend::set_texture_params(bool linear, bool clamp_s, bool clamp_t) {
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp_s ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp_t ? GL_CLAMP_TO_EDGE : GL_REPEAT);
}

GLuint GlBackend::texture_for(const gpu::Ge &ge, float uv_xform[4]) {
    const auto info = ge.texture_info();
    tex_width_ = static_cast<float>(info.width);
    tex_height_ = static_cast<float>(info.height);
    uv_xform[0] = 1.0f;
    uv_xform[1] = 1.0f;
    uv_xform[2] = 0.0f;
    uv_xform[3] = 0.0f;
    const bool indexed = info.format >= 4 && info.format <= 7;
    // A texture inside a framebuffer the GE has drawn: sample the render target itself.
    if ((info.address & 0x0F000000u) == 0x04000000u && !indexed) {
        if (RenderTarget *rt = find_target(info.address & 0x001FFFFFu, true)) {
            const std::uint32_t offset_pixels = ((info.address & 0x001FFFFFu) - rt->address) / rt->bytes_per_pixel();
            const float ox = static_cast<float>(offset_pixels % rt->stride), oy = static_cast<float>(offset_pixels / rt->stride);
            uv_xform[0] = tex_width_ / static_cast<float>(rt->width);
            uv_xform[1] = tex_height_ / static_cast<float>(rt->height);
            uv_xform[2] = ox / static_cast<float>(rt->width);
            uv_xform[3] = oy / static_cast<float>(rt->height);
            if (rt != current_) return rt->color;
            // Reading the target being drawn: sample a copy.
            const GLsizei w = static_cast<GLsizei>(rt->width) * scale_, h = static_cast<GLsizei>(rt->height) * scale_;
            gl_.BindTexture(GL_TEXTURE_2D, copy_texture_);
            if (copy_width_ != static_cast<std::uint32_t>(w) || copy_height_ != static_cast<std::uint32_t>(h)) {
                gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                copy_width_ = static_cast<std::uint32_t>(w);
                copy_height_ = static_cast<std::uint32_t>(h);
            }
            gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, copy_fbo_);
            gl_.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, copy_texture_, 0);
            gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo);
            gl_.Disable(GL_SCISSOR_TEST);
            gl_.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            gl_.BindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
            return copy_texture_;
        }
    }
    // Decoded textures, cached by parameters and checked against their content once per frame.
    std::uint64_t key = info.address;
    key = key * 1315423911u ^ (static_cast<std::uint64_t>(info.format) << 40u) ^ (static_cast<std::uint64_t>(info.width) << 20u) ^
          info.height ^ (static_cast<std::uint64_t>(info.stride) << 48u) ^ (info.swizzled ? 0x8000000000000000ull : 0u);
    if (indexed) key ^= static_cast<std::uint64_t>(ge.reg(kClutFormat)) * 0x9E3779B97F4A7C15ull;
    CachedTexture &t = textures_[key];
    t.last_use = frame_;
    const bool stale = t.texture == 0 || t.checked_frame != frame_ || (indexed && t.checked_clut != ge.clut_generation());
    if (stale) {
        const std::uint64_t hash = ge.texture_hash();
        t.checked_frame = frame_;
        t.checked_clut = ge.clut_generation();
        if (t.texture == 0 || hash != t.hash) {
            t.hash = hash;
            pixels_.resize(static_cast<std::size_t>(info.width) * info.height);
            ge.decode_texture(pixels_.data());
            if (interpolate_ && t.texture != 0) {
                // A recorded batch may replay with the old content: never overwrite a texture in place.
                retired_textures_.emplace_back(frame_, t.texture);
                t.texture = 0;
            }
            if (t.texture == 0) gl_.GenTextures(1, &t.texture);
            gl_.BindTexture(GL_TEXTURE_2D, t.texture);
            gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(info.width), static_cast<GLsizei>(info.height),
                           0, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
        }
    }
    return t.texture;
}

GLenum blend_factor(std::uint32_t f, bool source_side, std::uint32_t fixed, bool &uses_constant) {
    switch (f) {
    case 0: return source_side ? GL_DST_COLOR : GL_SRC_COLOR;
    case 1: return source_side ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 2: return GL_SRC_ALPHA;
    case 3: return GL_ONE_MINUS_SRC_ALPHA;
    case 4: return GL_DST_ALPHA;
    case 5: return GL_ONE_MINUS_DST_ALPHA;
    case 6: return GL_SRC_ALPHA;           // 2 * src alpha: approximated
    case 7: return GL_ONE_MINUS_SRC_ALPHA; // 1 - 2 * src alpha: approximated
    case 8: return GL_DST_ALPHA;
    case 9: return GL_ONE_MINUS_DST_ALPHA;
    default:
        if ((fixed & 0xFFFFFFu) == 0xFFFFFFu) return GL_ONE;
        if ((fixed & 0xFFFFFFu) == 0) return GL_ZERO;
        uses_constant = true;
        return GL_CONSTANT_COLOR;
    }
}

void GlBackend::apply_state(const gpu::Ge &ge) {
    DrawSetup setup;
    setup.key = state_key_;
    const std::uint32_t fb = ge.reg(kFramebufPtr) & 0x1FFFF0u;
    const std::uint32_t stride = ge.reg(kFramebufWidth) & 0x7FCu;
    const std::uint32_t format = ge.reg(kFramebufPixFormat) & 3u;
    const std::uint32_t sy2 = (ge.reg(kScissor2) >> 10u) & 0x3FFu;
    RenderTarget &rt = target_for(fb, stride == 0 ? 512u : stride, format, sy2 + 1u);
    current_ = &rt;
    rt.last_draw = frame_;
    through_ = (ge.reg(kVertexType) & (1u << 23u)) != 0;
    setup.through = through_;
    setup.target = rt.address;

    // Texture (may blit, so before the target is bound).
    const bool clear = (ge.reg(kClearMode) & 1u) != 0;
    const bool textured = !clear && (ge.reg(kTextureMapEnable) & 1u) != 0;
    tex_width_ = tex_height_ = 1.0f;
    if (textured) {
        gl_.ActiveTexture(GL_TEXTURE0);
        setup.texture = texture_for(ge, setup.uv_xform);
        setup.self_sampled = setup.texture == copy_texture_;
    }
    setup.tex_width = tex_width_;
    setup.tex_height = tex_height_;
    setup_ = setup;
    emit_state(setup, rt);
}

void GlBackend::emit_state(const DrawSetup &setup, RenderTarget &rt) {
    const auto R = [&](std::uint32_t reg) { return key_reg(setup.key, reg); };
    const std::uint32_t sx1 = R(kScissor1) & 0x3FFu, sy1 = (R(kScissor1) >> 10u) & 0x3FFu;
    const std::uint32_t sx2 = R(kScissor2) & 0x3FFu, sy2 = (R(kScissor2) >> 10u) & 0x3FFu;
    const bool through = setup.through;
    const float *uv_xform = setup.uv_xform;

    gl_.UseProgram(draw_program_);
    gl_.BindVertexArray(vao_);

    const bool clear = (R(kClearMode) & 1u) != 0;
    const bool textured = !clear && (R(kTextureMapEnable) & 1u) != 0;
    if (textured) {
        gl_.ActiveTexture(GL_TEXTURE0);
        gl_.BindTexture(GL_TEXTURE_2D, setup.texture);
        // Enhancement (off by default): bilinear filtering for 3D textures; 2D (through-mode) art stays as drawn.
        const bool linear = (R(kTexFilter) & 0x100u) != 0 || (smooth_textures_ && !through);
        set_texture_params(linear, (R(kTexWrap) & 1u) != 0, (R(kTexWrap) & 0x100u) != 0);
    }

    gl_.BindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    gl_.Viewport(0, 0, static_cast<GLsizei>(rt.width) * scale_, static_cast<GLsizei>(rt.height) * scale_);
    gl_.Enable(GL_SCISSOR_TEST);
    const std::uint32_t x2 = std::min(sx2, rt.width - 1u), y2 = std::min(sy2, rt.height - 1u);
    if (sx1 > x2 || sy1 > y2) {
        gl_.Scissor(0, 0, 0, 0);
    } else {
        gl_.Scissor(static_cast<GLint>(sx1) * scale_, static_cast<GLint>(sy1) * scale_,
                    static_cast<GLsizei>(x2 - sx1 + 1u) * scale_, static_cast<GLsizei>(y2 - sy1 + 1u) * scale_);
    }

    gl_.Uniform2f(u_.target, static_cast<float>(rt.width), static_cast<float>(rt.height));
    gl_.Uniform4f(u_.uv_xform, uv_xform[0], uv_xform[1], uv_xform[2], uv_xform[3]);
    gl_.Uniform1i(u_.clear, clear ? 1 : 0);
    gl_.Uniform1i(u_.tex_enable, textured ? 1 : 0);

    if (clear) {
        const std::uint32_t cm = R(kClearMode);
        const bool rgb = (cm & 0x100u) != 0, alpha = (cm & 0x200u) != 0, depth = (cm & 0x400u) != 0;
        gl_.Disable(GL_BLEND);
        gl_.ColorMask(rgb, rgb, rgb, alpha);
        gl_.Enable(GL_DEPTH_TEST);
        gl_.DepthFunc(GL_ALWAYS);
        gl_.DepthMask(depth ? GL_TRUE : GL_FALSE);
        gl_.Uniform1i(u_.alpha_func, 1);
        gl_.Uniform1i(u_.zrange_enable, 0);
        return;
    }

    const std::uint32_t func = R(kTexFunc);
    gl_.Uniform1i(u_.tex_func, static_cast<GLint>(func & 7u));
    gl_.Uniform1i(u_.tex_rgba, (func & 0x100u) != 0 ? 1 : 0);
    gl_.Uniform1i(u_.tex_double, (func & 0x10000u) != 0 ? 1 : 0);
    float env[3];
    unpack_rgb(R(kTexEnvColor), env);
    gl_.Uniform3f(u_.env, env[0], env[1], env[2]);
    const bool fog = !through && (R(kFogEnable) & 1u) != 0;
    gl_.Uniform1i(u_.fog_enable, fog ? 1 : 0);
    float fog_color[3];
    unpack_rgb(R(kFogColor), fog_color);
    gl_.Uniform3f(u_.fog_color, fog_color[0], fog_color[1], fog_color[2]);

    if ((R(kAlphaTestEnable) & 1u) != 0) {
        const std::uint32_t at = R(kAlphaTest);
        gl_.Uniform1i(u_.alpha_func, static_cast<GLint>(at & 7u));
        gl_.Uniform1i(u_.alpha_ref, static_cast<GLint>((at >> 8u) & 0xFFu));
        gl_.Uniform1i(u_.alpha_mask, static_cast<GLint>((at >> 16u) & 0xFFu));
    } else {
        gl_.Uniform1i(u_.alpha_func, 1);
    }
    const std::uint32_t minz = R(kMinZ) & 0xFFFFu, maxz = R(kMaxZ) & 0xFFFFu;
    const bool zrange = !through && maxz != 0 && (minz != 0 || maxz != 0xFFFFu);
    gl_.Uniform1i(u_.zrange_enable, zrange ? 1 : 0);
    gl_.Uniform2f(u_.zrange, static_cast<float>(minz), static_cast<float>(maxz));

    // Depth.
    if ((R(kZTestEnable) & 1u) != 0) {
        static constexpr GLenum funcs[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
        gl_.Enable(GL_DEPTH_TEST);
        gl_.DepthFunc(funcs[R(kZTest) & 7u]);
        gl_.DepthMask((R(kZWriteDisable) & 1u) == 0 ? GL_TRUE : GL_FALSE);
    } else {
        gl_.Disable(GL_DEPTH_TEST);
        gl_.DepthMask(GL_FALSE);
    }

    // Blending.
    if ((R(kAlphaBlendEnable) & 1u) != 0) {
        const std::uint32_t mode = R(kBlendMode);
        bool constant_a = false, constant_b = false;
        const std::uint32_t fix_a = R(kBlendFixedA), fix_b = R(kBlendFixedB);
        const GLenum src = blend_factor(mode & 0xFu, true, fix_a, constant_a);
        GLenum dst = blend_factor((mode >> 4u) & 0xFu, false, fix_b, constant_b);
        std::uint32_t constant = constant_a ? fix_a : fix_b;
        if (constant_a && constant_b && (fix_a & 0xFFFFFFu) != (fix_b & 0xFFFFFFu)) {
            // One constant color only: B = 1 - A is the usual case.
            dst = GL_ONE_MINUS_CONSTANT_COLOR;
            constant = fix_a;
        }
        if (constant_a || constant_b) {
            float c[3];
            unpack_rgb(constant, c);
            gl_.BlendColor(c[0], c[1], c[2], 1.0f);
        }
        static constexpr GLenum equations[6] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX, GL_MAX};
        const std::uint32_t eq = std::min<std::uint32_t>((mode >> 8u) & 0xFu, 5u);
        gl_.Enable(GL_BLEND);
        gl_.BlendEquationSeparate(equations[eq], GL_FUNC_ADD);
        gl_.BlendFuncSeparate(src, dst, GL_ONE, GL_ZERO);
    } else {
        gl_.Disable(GL_BLEND);
    }

    // Write masks (a channel is written unless every bit of it is masked).
    const std::uint32_t mask = R(kMaskRgb);
    const bool keep_alpha = (R(kStencilTestEnable) & 1u) != 0 || (R(kMaskAlpha) & 0xFFu) == 0xFFu;
    gl_.ColorMask((mask & 0xFFu) != 0xFFu, ((mask >> 8u) & 0xFFu) != 0xFFu, ((mask >> 16u) & 0xFFu) != 0xFFu, !keep_alpha);
}

void GlBackend::flush() {
    draw_batch();
    // Testing aid: MHF_GL_READBACK=1 copies drawn targets back into guest VRAM after every list, so that tests that
    // read pixels (homebrew/getest) can check the hardware renderer too.
    static const bool readback = [] {
        const char *v = std::getenv("MHF_GL_READBACK");
        return v != nullptr && v[0] == '1';
    }();
    if (readback) read_back();
}

void GlBackend::read_back() {
    std::vector<std::uint8_t> rgba;
    for (auto &[address, rt] : targets_) {
        if (rt.last_draw != frame_) continue;
        const GLsizei w = static_cast<GLsizei>(rt.width) * scale_, h = static_cast<GLsizei>(rt.height) * scale_;
        rgba.resize(static_cast<std::size_t>(w) * h * 4u);
        gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, rt.fbo);
        gl_.PixelStorei(GL_PACK_ALIGNMENT, 1);
        gl_.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        const std::uint32_t bpp = rt.bytes_per_pixel();
        for (std::uint32_t y = 0; y < rt.height; ++y) {
            std::uint8_t *row = memory_.raw_pointer(0x04000000u + rt.address + y * rt.stride * bpp, rt.width * bpp);
            if (row == nullptr) continue;
            for (std::uint32_t x = 0; x < rt.width; ++x) {
                const std::uint8_t *p = rgba.data() + (static_cast<std::size_t>(y) * scale_ * w + static_cast<std::size_t>(x) * scale_) * 4u;
                if (bpp == 4) {
                    std::memcpy(row + x * 4u, p, 4);
                    continue;
                }
                std::uint16_t v;
                if (rt.format == 0) v = static_cast<std::uint16_t>((p[0] >> 3) | ((p[1] >> 2) << 5) | ((p[2] >> 3) << 11));
                else if (rt.format == 1) v = static_cast<std::uint16_t>((p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10) | ((p[3] >> 7) << 15));
                else v = static_cast<std::uint16_t>((p[0] >> 4) | ((p[1] >> 4) << 4) | ((p[2] >> 4) << 8) | ((p[3] >> 4) << 12));
                std::memcpy(row + x * 2u, &v, 2);
            }
        }
    }
    have_state_ = false;
}

void GlBackend::draw_batch() {
    if (batch_.empty()) return;
    if (interpolate_) {
        if (tape_.size() >= 20000) { // a game that never flips: do not keep everything
            tape_.clear();
            touched_.insert(~0u);
        }
        tape_.push_back({setup_, batch_, batch_origins_});
    }
    draw_vertices(batch_);
    batch_.clear();
    batch_origins_.clear();
}

void GlBackend::draw_vertices(const std::vector<GpuVertex> &vertices) {
    gl_.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl_.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(GpuVertex)), vertices.data(),
                   GL_STREAM_DRAW);
    gl_.DrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
}

// ---------------------------------------------------------------- transfers and presentation

void GlBackend::upload_rect(RenderTarget &rt, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    const std::uint32_t bpp = rt.bytes_per_pixel();
    pixels_.resize(static_cast<std::size_t>(w) * h);
    for (std::uint32_t row = 0; row < h; ++row) {
        const std::uint32_t address = 0x04000000u + rt.address + ((y + row) * rt.stride + x) * bpp;
        const std::uint8_t *p = memory_.raw_pointer(address, w * bpp);
        for (std::uint32_t col = 0; col < w; ++col) {
            pixels_[static_cast<std::size_t>(row) * w + col] = p != nullptr ? framebuffer_pixel(p + col * bpp, rt.format) : 0u;
        }
    }
    gl_.BindTexture(GL_TEXTURE_2D, upload_texture_);
    gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, pixels_.data());
    set_texture_params(false, true, true);
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, copy_fbo_);
    gl_.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, upload_texture_, 0);
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, rt.fbo);
    gl_.Disable(GL_SCISSOR_TEST);
    gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl_.BlitFramebuffer(0, 0, static_cast<GLint>(w), static_cast<GLint>(h), static_cast<GLint>(x) * scale_,
                        static_cast<GLint>(y) * scale_, static_cast<GLint>(x + w) * scale_, static_cast<GLint>(y + h) * scale_,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

void GlBackend::block_transfer(std::uint32_t src, std::uint32_t /*src_stride: the target's own*/, std::uint32_t sx, std::uint32_t sy,
                               std::uint32_t dst, std::uint32_t dst_stride, std::uint32_t dx, std::uint32_t dy,
                               std::uint32_t width, std::uint32_t height, std::uint32_t bpp) {
    draw_batch();
    have_state_ = false; // GL state is changed below
    const bool src_vram = (src & 0x0F000000u) == 0x04000000u, dst_vram = (dst & 0x0F000000u) == 0x04000000u;
    if (!dst_vram) return; // copies into RAM are already done in memory
    const std::uint32_t format = bpp == 4 ? 3u : 1u;
    RenderTarget *from = src_vram ? find_target(src & 0x001FFFFFu, true) : nullptr;
    if (from != nullptr && from->bytes_per_pixel() == bpp) {
        // Framebuffer to framebuffer (or to a texture area): copy on the GPU at full resolution.
        const std::uint32_t src_offset = ((src & 0x001FFFFFu) - from->address) / bpp;
        const std::uint32_t fx = src_offset % from->stride + sx, fy = src_offset / from->stride + sy;
        RenderTarget &to = target_for(dst & 0x001FFFF0u, dst_stride, format, dy + height);
        to.last_draw = frame_;
        touched_.insert(to.address);
        gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, from->fbo);
        gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, to.fbo);
        gl_.Disable(GL_SCISSOR_TEST);
        gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        const GLint s = scale_;
        if (from == &to) {
            // Source and destination in one target: copy the source out first (overlapping blits are undefined).
            const GLsizei w = static_cast<GLsizei>(from->width) * s, h = static_cast<GLsizei>(from->height) * s;
            gl_.BindTexture(GL_TEXTURE_2D, copy_texture_);
            if (copy_width_ != static_cast<std::uint32_t>(w) || copy_height_ != static_cast<std::uint32_t>(h)) {
                gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                copy_width_ = static_cast<std::uint32_t>(w);
                copy_height_ = static_cast<std::uint32_t>(h);
            }
            gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, copy_fbo_);
            gl_.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, copy_texture_, 0);
            gl_.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, copy_fbo_);
            gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, to.fbo);
        }
        gl_.BlitFramebuffer(static_cast<GLint>(fx) * s, static_cast<GLint>(fy) * s, static_cast<GLint>(fx + width) * s,
                            static_cast<GLint>(fy + height) * s, static_cast<GLint>(dx) * s, static_cast<GLint>(dy) * s,
                            static_cast<GLint>(dx + width) * s, static_cast<GLint>(dy + height) * s, GL_COLOR_BUFFER_BIT,
                            GL_NEAREST);
        return;
    }
    // CPU-made pixels copied into a framebuffer the GE draws to: bring them into the render target.
    if (RenderTarget *to = find_target(dst & 0x001FFFFFu, true)) {
        touched_.insert(to->address);
        const std::uint32_t offset = ((dst & 0x001FFFFFu) - to->address) / to->bytes_per_pixel();
        upload_rect(*to, offset % to->stride + dx, offset / to->stride + dy, width, height);
    }
}

void GlBackend::present(std::uint32_t address, std::uint32_t stride, std::uint32_t pixel_format) {
    draw_batch();
    have_state_ = false;
    flush_pictures(); // pictures of the previous flip that never got their turn
    const std::uint32_t offset = address & 0x001FFFFFu;
    GLuint texture = 0;
    float uv_max[2] = {1, 1};
    RenderTarget *drawn = nullptr;
    const auto it = targets_.find(offset);
    // A target the GE drew since the last flip (or the one before) shows what the game rendered; otherwise the CPU
    // made this screen (movies, loading screens), so show guest memory.
    if (it != targets_.end() && it->second.last_draw + 1 >= frame_) {
        drawn = &it->second;
        texture = it->second.color;
        uv_max[0] = 480.0f / static_cast<float>(it->second.width);
        uv_max[1] = 272.0f / static_cast<float>(it->second.height);
    } else if ((address & 0x0F000000u) == 0x04000000u || (address & 0x0F000000u) == 0x08000000u) {
        const std::uint32_t bpp = pixel_format == 3 ? 4u : 2u;
        stride = std::max(stride, 480u);
        pixels_.resize(480u * 272u);
        const std::uint8_t *base = memory_.raw_pointer(address, static_cast<std::size_t>(stride) * 271u * bpp + 480u * bpp);
        for (std::uint32_t y = 0; y < 272; ++y) {
            for (std::uint32_t x = 0; x < 480; ++x) {
                pixels_[y * 480u + x] = base != nullptr ? framebuffer_pixel(base + (y * stride + x) * bpp, pixel_format) : 0u;
            }
        }
        gl_.BindTexture(GL_TEXTURE_2D, display_texture_);
        gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 480, 272, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
        texture = display_texture_;
    }

    if (interpolate_) {
        if (drawn == nullptr || !present_interpolated(*drawn, uv_max)) show(texture, uv_max[0], uv_max[1]);
        if (drawn == nullptr) previous_valid_ = false;
    } else {
        show(texture, uv_max[0], uv_max[1]);
    }
    finish_flip();
}

// Puts a picture on the window: letterboxed to the PSP's 480:272, with the dialog overlay on top.
void GlBackend::show(GLuint texture, float uv_max_x, float uv_max_y) {
    int window_w = 0, window_h = 0;
    SDL_GetWindowSizeInPixels(window_, &window_w, &window_h);
    gl_.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_.Disable(GL_SCISSOR_TEST);
    gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl_.Viewport(0, 0, window_w, window_h);
    gl_.ClearColor(0, 0, 0, 1);
    gl_.Clear(GL_COLOR_BUFFER_BIT);
    if (texture != 0 && window_w > 0 && window_h > 0) {
        int w = window_w, h = window_w * 272 / 480;
        if (h > window_h) {
            h = window_h;
            w = window_h * 480 / 272;
        }
        gl_.Viewport((window_w - w) / 2, (window_h - h) / 2, w, h);
        gl_.Disable(GL_DEPTH_TEST);
        gl_.Disable(GL_BLEND);
        gl_.UseProgram(present_program_);
        gl_.Uniform1i(present_tex_, 0);
        gl_.Uniform2f(present_uv_max_, uv_max_x, uv_max_y);
        gl_.ActiveTexture(GL_TEXTURE0);
        gl_.BindTexture(GL_TEXTURE_2D, texture);
        set_texture_params(true, true, true);
        gl_.BindVertexArray(empty_vao_);
        gl_.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    if (window_w > 0 && window_h > 0) {
        int w = window_w, h = window_w * 272 / 480;
        if (h > window_h) {
            h = window_h;
            w = window_h * 480 / 272;
        }
        gl_.Viewport((window_w - w) / 2, (window_h - h) / 2, w, h);
        draw_overlay();
    }
    dump_window(window_w, window_h);
    SDL_GL_SwapWindow(window_);
    ++shown_;
}

void GlBackend::finish_flip() {
    ++frame_;
    tape_.clear();
    touched_.clear();
    cameras_.clear();
    camera_ = kNoCamera;
    draw_uses_.clear();
    for (auto i = retired_textures_.begin(); i != retired_textures_.end();) {
        if (i->first + 4 < frame_) {
            gl_.DeleteTextures(1, &i->second);
            i = retired_textures_.erase(i);
        } else {
            ++i;
        }
    }

    // Forget textures not used for a while.
    if ((frame_ & 255u) == 0) {
        for (auto i = textures_.begin(); i != textures_.end();) {
            if (i->second.last_use + 600 < frame_) {
                gl_.DeleteTextures(1, &i->second.texture);
                i = textures_.erase(i);
            } else {
                ++i;
            }
        }
    }
}

void GlBackend::ensure_slot(Slot &slot, GLsizei w, GLsizei h) {
    if (slot.fbo != 0 && slot.width == w && slot.height == h) return;
    if (slot.fbo != 0) {
        gl_.DeleteFramebuffers(1, &slot.fbo);
        gl_.DeleteTextures(1, &slot.texture);
    }
    gl_.GenTextures(1, &slot.texture);
    gl_.BindTexture(GL_TEXTURE_2D, slot.texture);
    gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    set_texture_params(true, true, true);
    gl_.GenFramebuffers(1, &slot.fbo);
    gl_.BindFramebuffer(GL_FRAMEBUFFER, slot.fbo);
    gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.texture, 0);
    slot.width = w;
    slot.height = h;
    previous_valid_ = false; // the older picture, if it was this slot, is gone
}

namespace {

// How far (PSP pixels) a vertex may move between two flips and still be taken as the same point: beyond that it is
// a cut, an object appearing, or a different thing drawn in the same place.
constexpr float kMaxMove = 32.0f;

// Texture coordinates travel with a point of the mesh, so two vertices that are the same point have the same ones
// (up to a small tolerance); when they differ the pairing is wrong, or the texture is animated.
constexpr float kMaxUvChange = 0.01f;

bool clearing(const DrawSetup &s) { return (key_reg(s.key, kClearMode) & 1u) != 0; }

// Draw state compared register by register, except the draw buffer address.
bool same_state(const DrawKey &a, const DrawKey &b) {
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i < kStateRegs.size() && kStateRegs[i] == kFramebufPtr) continue; // the draw buffer alternates every flip
        if (a[i] != b[i]) return false;
    }
    return true;
}

// The same on-screen thing in two flips: same draw state (texture, blending...; the game alternates two draw buffers,
// so which one it went to does not count), same texture object (a texture whose content changed is another one, see
// texture_for) and the same number of vertices.
bool same_batch(const TapeEntry &a, const TapeEntry &b) {
    return a.vertices.size() == b.vertices.size() && a.setup.through == b.setup.through &&
           same_state(a.setup.key, b.setup.key) && a.setup.texture == b.setup.texture && a.setup.uv_xform[0] == b.setup.uv_xform[0] &&
           a.setup.uv_xform[1] == b.setup.uv_xform[1] && a.setup.uv_xform[2] == b.setup.uv_xform[2] &&
           a.setup.uv_xform[3] == b.setup.uv_xform[3];
}

// Screen-space (2D) triangles move rigidly: a sprite slides, it does not stretch. Only such triangles, whose texture
// coordinates stayed the same too, are placed between their two positions.
constexpr float kMaxRigidSkew = 1.5f;

enum class Move { Still, Blend, Refuse };

Move classify_triangle(const GpuVertex *before, const GpuVertex *now) {
    float dx[3], dy[3];
    bool still = true;
    for (int k = 0; k < 3; ++k) {
        dx[k] = now[k].x - before[k].x;
        dy[k] = now[k].y - before[k].y;
        if (std::fabs(now[k].u - before[k].u) > kMaxUvChange || std::fabs(now[k].v - before[k].v) > kMaxUvChange) {
            return Move::Refuse;
        }
        if (std::fabs(dx[k]) > kMaxMove || std::fabs(dy[k]) > kMaxMove) return Move::Refuse;
        still = still && std::fabs(dx[k]) < 0.01f && std::fabs(dy[k]) < 0.01f;
    }
    if (still) return Move::Still;
    for (int k = 1; k < 3; ++k) {
        if (std::fabs(dx[k] - dx[0]) > kMaxRigidSkew || std::fabs(dy[k] - dy[0]) > kMaxRigidSkew) return Move::Refuse;
    }
    return Move::Blend;
}

// `out` holds this flip's vertices; moves each triangle back toward `before` by (1 - t) when it slid rigidly.
void blend_vertices(const std::vector<GpuVertex> &before, std::span<GpuVertex> out, float t) {
    for (std::size_t i = 0; i + 2 < out.size(); i += 3) {
        if (classify_triangle(&before[i], &out[i]) != Move::Blend) continue;
        for (std::size_t k = 0; k < 3; ++k) {
            out[i + k].x = before[i + k].x + (out[i + k].x - before[i + k].x) * t;
            out[i + k].y = before[i + k].y + (out[i + k].y - before[i + k].y) * t;
        }
    }
}

// Pairs the 2D batches of this flip with the previous flip's (looking ahead a few batches) and measures how much of the
// textured 2D content changed in a way an in-between picture cannot show: sprites that appeared, vanished or jumped.
// The previous picture is the base of every in-between picture, so such sprites would show twice for a moment (item
// icons of a list, text). Returns that count in sprites; `partner` gets, per batch of `now`, its pair or -1.
std::size_t analyze_2d(const std::vector<TapeEntry> &now, const std::vector<TapeEntry> &before, std::vector<int> &partner) {
    partner.assign(now.size(), -1);
    std::vector<bool> used(before.size(), false);
    std::size_t next = 0, changed = 0;
    for (std::size_t i = 0; i < now.size(); ++i) {
        const TapeEntry &e = now[i];
        if (!e.setup.through) continue;
        for (std::size_t look = 0; look < 4 && next + look < before.size(); ++look) {
            if (same_batch(e, before[next + look])) {
                partner[i] = static_cast<int>(next + look);
                used[next + look] = true;
                next += look + 1;
                break;
            }
        }
    }
    for (std::size_t i = 0; i < now.size(); ++i) {
        const TapeEntry &e = now[i];
        if (!e.setup.through || clearing(e.setup) || e.setup.texture == 0) continue;
        if (partner[i] < 0) {
            changed += e.vertices.size() / 6;
            continue;
        }
        const TapeEntry &b = before[static_cast<std::size_t>(partner[i])];
        std::size_t refused = 0;
        for (std::size_t v = 0; v + 2 < e.vertices.size(); v += 3) {
            refused += classify_triangle(&b.vertices[v], &e.vertices[v]) == Move::Refuse ? 1u : 0u;
        }
        changed += (refused + 1) / 2;
    }
    for (std::size_t j = 0; j < before.size(); ++j) {
        const TapeEntry &b = before[j];
        if (!used[j] && b.setup.through && !clearing(b.setup) && b.setup.texture != 0) changed += b.vertices.size() / 6;
    }
    return changed;
}

// More changed sprites than this, in a flip that does not clear the screen, and the flip is shown as the game drew it
// (menus, item lists, dialogs over the previous picture). A flip that clears the colors first (every 3D scene, with
// its HUD) draws everything again over nothing, so the previous picture cannot show through and nothing is skipped.
constexpr std::size_t kMaxChangedSprites = 6;

bool clears_screen(const std::vector<TapeEntry> &batches) {
    for (const TapeEntry &e : batches) {
        const std::uint32_t mode = key_reg(e.setup.key, kClearMode);
        if ((mode & 1u) != 0 && (mode & 0x100u) != 0) return true; // clear mode with the color buffer
    }
    return false;
}

bool nearly_equal(float a, float b) { return std::fabs(a - b) <= 1e-3f * std::max(1.0f, std::fabs(a)); }

// The camera of the previous flip that `now` continues: same projection and viewport, and the nearest view matrix,
// which may only have turned a little (a cut is not interpolated).
const Camera *continued_camera(const Camera &now, const std::vector<Camera> &before) {
    const Camera *best = nullptr;
    float best_distance = 0;
    for (const Camera &c : before) {
        bool same_lens = true;
        for (std::size_t i = 0; i < c.proj.size() && same_lens; ++i) same_lens = nearly_equal(now.proj[i], c.proj[i]);
        for (std::size_t i = 0; i < c.viewport.size() && same_lens; ++i) same_lens = nearly_equal(now.viewport[i], c.viewport[i]);
        if (!same_lens) continue;
        float turn = 0, distance = 0;
        for (std::size_t i = 0; i < 12; ++i) {
            const float d = std::fabs(now.view[i] - c.view[i]);
            if (i < 9) turn += d;
            distance += d;
        }
        if (turn > 0.75f) continue;
        if (best == nullptr || distance < best_distance) {
            best = &c;
            best_distance = distance;
        }
    }
    return best;
}

Camera between(const Camera &a, const Camera &b, float t) {
    Camera c = b;
    for (std::size_t i = 0; i < c.view.size(); ++i) c.view[i] = a.view[i] + (b.view[i] - a.view[i]) * t;
    for (std::size_t i = 0; i < c.proj.size(); ++i) c.proj[i] = a.proj[i] + (b.proj[i] - a.proj[i]) * t;
    return c;
}

// World point to screen (x, y in PSP pixels, z 0..65535, 1/w), like the GE; false behind the near plane.
bool project(const Camera &c, const float p[3], float out[4]) {
    float view[3];
    for (std::size_t i = 0; i < 3; ++i) {
        view[i] = p[0] * c.view[i] + p[1] * c.view[3 + i] + p[2] * c.view[6 + i] + c.view[9 + i];
    }
    float clip[4];
    for (std::size_t i = 0; i < 4; ++i) {
        clip[i] = view[0] * c.proj[i] + view[1] * c.proj[4 + i] + view[2] * c.proj[8 + i] + c.proj[12 + i];
    }
    if (!(clip[3] > 0.0f) || !(clip[2] > -clip[3])) return false;
    const float inv_w = 1.0f / clip[3];
    out[0] = clip[0] * inv_w * c.viewport[0] + c.viewport[3] - c.viewport[6];
    out[1] = clip[1] * inv_w * c.viewport[1] + c.viewport[4] - c.viewport[7];
    out[2] = clip[2] * inv_w * c.viewport[2] + c.viewport[5];
    out[3] = inv_w;
    return std::isfinite(out[0]) && std::isfinite(out[1]);
}

// Places a point for an in-between picture: seen through the camera between the two flips and, when the same point
// was drawn in the previous flip (`was`), moved back toward where it was then (things that move on their own).
bool place(const float world[3], const float *was, const Camera &camera, float t, float out[4]) {
    if (!project(camera, world, out)) return false;
    if (was == nullptr) return true;
    const float moved[3] = {was[0] + (world[0] - was[0]) * t, was[1] + (world[1] - was[1]) * t,
                            was[2] + (world[2] - was[2]) * t};
    // A point that seems to have jumped far is another thing drawn from the same data: keep it where it is now.
    float s[4];
    if (project(camera, moved, s) && std::fabs(s[0] - out[0]) <= kMaxMove && std::fabs(s[1] - out[1]) <= kMaxMove) {
        std::copy(s, s + 4, out);
    }
    return true;
}

} // namespace

// Moves a recorded 3D batch to phase `t` between the previous flip and this one, with `cameras` the cameras at that
// phase. Each triangle is placed again as a whole, or left where it was drawn when one of its points cannot be (no
// camera, behind the camera).
void GlBackend::place_again(const TapeEntry &entry, const std::vector<Camera> &cameras, std::span<GpuVertex> vertices,
                            float t) const {
    if (entry.origins.size() != vertices.size()) return;
    for (std::size_t i = 0; i + 2 < vertices.size(); i += 3) {
        GpuVertex placed[3];
        bool ok = true;
        for (std::size_t k = 0; k < 3 && ok; ++k) {
            const Origin &o = entry.origins[i + k];
            placed[k] = vertices[i + k];
            if (o.camera >= cameras.size()) { // also kNoCamera
                ok = false;
                break;
            }
            const Camera &camera = cameras[o.camera];
            float sx[4];
            ok = place(o.x_world, o.x_moved ? o.x_before : nullptr, camera, t, sx);
            if (!ok) break;
            placed[k].x = sx[0];
            if (o.sprite) {
                float sy[4];
                ok = place(o.y_world, o.y_moved ? o.y_before : nullptr, camera, t, sy);
                placed[k].y = sy[1];
            } else {
                placed[k].y = sx[1];
                placed[k].z = sx[2];
                placed[k].inv_w = sx[3];
            }
        }
        if (ok) std::copy(placed, placed + 3, vertices.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void GlBackend::render_between(const RenderTarget &shown, const std::vector<TapeEntry> &now,
                               const std::vector<TapeEntry> &before_batches, const Slot &before_slot, float t) {
    const GLsizei w = static_cast<GLsizei>(shown.width) * scale_, h = static_cast<GLsizei>(shown.height) * scale_;
    if (scratch_.fbo == 0 || scratch_.width != shown.width || scratch_.height != shown.height) {
        if (scratch_.fbo != 0) {
            gl_.DeleteFramebuffers(1, &scratch_.fbo);
            gl_.DeleteTextures(1, &scratch_.color);
            gl_.DeleteRenderbuffers(1, &scratch_.depth);
        }
        scratch_ = RenderTarget{};
        scratch_.address = shown.address;
        scratch_.stride = shown.stride;
        scratch_.width = shown.width;
        scratch_.format = shown.format;
        allocate(scratch_, shown.height);
    }
    // Start from the previous picture, with a fresh depth buffer.
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, before_slot.fbo);
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, scratch_.fbo);
    gl_.Disable(GL_SCISSOR_TEST);
    gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl_.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl_.DepthMask(GL_TRUE);
    gl_.Clear(GL_DEPTH_BUFFER_BIT);

    std::vector<Camera> cameras(flip_.cameras.size());
    for (std::size_t i = 0; i < cameras.size(); ++i) cameras[i] = between(flip_.cameras_before[i], flip_.cameras[i], t);
    using Clock = std::chrono::steady_clock;
    const auto us = [](Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<double, std::micro>(to - from).count();
    };
    // All the vertices of the picture first (one upload), then the batches draw from ranges of them.
    const auto t_calc = Clock::now();
    staging_.clear();
    ranges_.assign(now.size(), Range{});
    for (std::size_t i = 0; i < now.size(); ++i) {
        const TapeEntry &entry = now[i];
        ranges_[i] = Range{staging_.size(), entry.vertices.size()};
        staging_.insert(staging_.end(), entry.vertices.begin(), entry.vertices.end());
    }
    // Placing the vertices again reads shared data only and writes disjoint ranges of the staging buffer, so big
    // pictures are split between a few threads (the drawing thread takes the first share).
    const auto place_batches = [&](std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to; ++i) {
            const TapeEntry &entry = now[i];
            const std::span<GpuVertex> vertices(staging_.data() + ranges_[i].first, ranges_[i].count);
            if (entry.setup.through) {
                const int partner = i < flip_.partner.size() ? flip_.partner[i] : -1;
                if (partner >= 0 && !clearing(entry.setup)) {
                    blend_vertices(before_batches[static_cast<std::size_t>(partner)].vertices, vertices, t);
                }
            } else {
                place_again(entry, cameras, vertices, t);
            }
        }
    };
    const unsigned cores = std::thread::hardware_concurrency();
    const std::size_t threads = staging_.size() >= 12000 && cores >= 3 ? std::min<std::size_t>(4, cores - 1) : 1;
    if (threads <= 1) {
        place_batches(0, now.size());
    } else {
        std::vector<std::future<void>> helpers;
        const std::size_t share = staging_.size() / threads + 1;
        std::size_t from = 0, end_of_first = 0;
        for (std::size_t k = 0; k < threads && from < now.size(); ++k) {
            std::size_t to = from, taken = 0;
            while (to < now.size() && (taken < share || k + 1 == threads)) taken += ranges_[to++].count;
            if (k == 0) end_of_first = to;
            else helpers.push_back(std::async(std::launch::async, place_batches, from, to));
            from = to;
        }
        place_batches(0, end_of_first);
        for (auto &helper : helpers) helper.get();
    }
    const auto t_upload = Clock::now();
    gl_.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl_.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(staging_.size() * sizeof(GpuVertex)), staging_.data(),
                   GL_STREAM_DRAW);
    cost_.calc_us += us(t_calc, t_upload);
    cost_.upload_us += us(t_upload, Clock::now());
    for (std::size_t i = 0; i < now.size(); ++i) {
        if (ranges_[i].count == 0) continue;
        const auto t_state = Clock::now();
        emit_state(now[i].setup, scratch_);
        const auto t_draw = Clock::now();
        gl_.DrawArrays(GL_TRIANGLES, static_cast<GLint>(ranges_[i].first), static_cast<GLsizei>(ranges_[i].count));
        cost_.state_us += us(t_state, t_draw);
        cost_.draw_us += us(t_draw, Clock::now());
    }
    ++cost_.pictures;
    cost_.batches += now.size();
    cost_.vertices += staging_.size();
    have_state_ = false;
}

void GlBackend::show_picture(const Picture &picture) {
    if (picture.phase >= 1.0f) {
        show(flip_.final_texture, flip_.uv_max[0], flip_.uv_max[1]);
        return;
    }
    RenderTarget shown;
    shown.address = flip_.address;
    shown.width = flip_.width;
    shown.height = flip_.height;
    shown.stride = flip_.stride;
    shown.format = flip_.format;
    render_between(shown, *flip_.now, flip_.before, slots_[flip_.before_slot], picture.phase);
    show(scratch_.color, flip_.uv_max[0], flip_.uv_max[1]);
}

// Called by the GE worker when the next picture is due.
void GlBackend::tick() {
    while (!pictures_.empty()) {
        const Picture picture = pictures_.front();
        pictures_.pop_front();
        // Behind schedule (the GE had other work): in-between pictures that are already late are dropped.
        if (picture.phase < 1.0f && std::chrono::steady_clock::now() - picture.due > flip_.slack) {
            ++stats_.late;
            continue;
        }
        show_picture(picture);
        return;
    }
}

void GlBackend::flush_pictures() {
    while (!pictures_.empty()) {
        const Picture picture = pictures_.front();
        pictures_.pop_front();
        if (picture.phase >= 1.0f) show_picture(picture);
    }
}

// Schedules the in-between pictures, the first one due at once, ending with the flip's own picture. Returns false
// (nothing scheduled) when this flip cannot be interpolated; the caller then shows it as usual. Either way the picture
// is kept for the next flip.
bool GlBackend::present_interpolated(RenderTarget &shown, const float uv_max[2]) {
    const auto now = std::chrono::steady_clock::now();
    const auto interval = now - last_flip_;
    last_flip_ = now;

    const GLsizei w = static_cast<GLsizei>(shown.width) * scale_, h = static_cast<GLsizei>(shown.height) * scale_;
    ensure_slot(slots_[0], w, h);
    ensure_slot(slots_[1], w, h);

    // The batches that drew to the picture being shown, and whether anything else wrote to it.
    std::vector<TapeEntry> mine;
    bool self_sampled = false;
    for (TapeEntry &e : tape_) {
        if (e.setup.target != shown.address) continue;
        self_sampled = self_sampled || e.setup.self_sampled;
        mine.push_back(std::move(e));
    }
    const bool copied = touched_.count(shown.address) != 0 || touched_.count(~0u) != 0;

    // Keep this picture for the next flip.
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, shown.fbo);
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, slots_[fill_slot_].fbo);
    gl_.Disable(GL_SCISSOR_TEST);
    gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl_.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    const auto ms = std::chrono::duration<double, std::milli>(interval).count();
    bool done = false;
    std::vector<int> partner;
    if (!previous_valid_) {
        ++stats_.skipped_first;
    } else if (copied || self_sampled) {
        ++stats_.skipped_copy;
    } else if (ms < 22.0 || ms > 150.0) {
        ++stats_.skipped_timing; // already fast, or a stall: nothing to fill in
    } else if (mine.empty() || previous_.empty()) {
        ++stats_.skipped_shape;
    } else if (analyze_2d(mine, previous_, partner) > kMaxChangedSprites && !clears_screen(mine)) {
        ++stats_.skipped_ui; // lists, menus and dialogs: sprites appear, vanish or jump
    } else {
        flip_.partner = std::move(partner);
        flip_.before = std::move(previous_);
        flip_.cameras = cameras_;
        flip_.cameras_before.clear();
        for (const Camera &c : cameras_) {
            const Camera *before = continued_camera(c, previous_cameras_);
            flip_.cameras_before.push_back(before != nullptr ? *before : c);
        }
        // Where each point of this flip was in the previous one (objects that move on their own).
        std::size_t points = 0;
        for (const TapeEntry &e : flip_.before) points += e.origins.size() * 2;
        std::size_t capacity = 1024;
        while (capacity < points * 2) capacity <<= 1;
        point_table_.assign(capacity, PointSlot{});
        const std::size_t mask = capacity - 1;
        const auto slot_of = [mask](std::uint64_t key) {
            return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> 20u) & mask;
        };
        const auto insert_point = [&](std::uint64_t key, const float *position) {
            for (std::size_t i = slot_of(key);; i = (i + 1) & mask) {
                if (point_table_[i].key == 0) {
                    point_table_[i] = PointSlot{key, position};
                    return;
                }
                if (point_table_[i].key == key) return;
            }
        };
        const auto find_point = [&](std::uint64_t key) -> const float * {
            for (std::size_t i = slot_of(key);; i = (i + 1) & mask) {
                if (point_table_[i].key == key) return point_table_[i].position;
                if (point_table_[i].key == 0) return nullptr;
            }
        };
        for (const TapeEntry &e : flip_.before) {
            for (const Origin &o : e.origins) {
                if (o.x_key != 0) insert_point(o.x_key, o.x_world);
                if (o.y_key != 0 && o.y_key != o.x_key) insert_point(o.y_key, o.y_world);
            }
        }
        for (TapeEntry &e : mine) {
            for (Origin &o : e.origins) {
                if (o.x_key != 0) {
                    const float *was = find_point(o.x_key);
                    o.x_moved = was != nullptr;
                    if (o.x_moved) std::copy(was, was + 3, o.x_before);
                }
                if (o.y_key != 0) {
                    const float *was = find_point(o.y_key);
                    o.y_moved = was != nullptr;
                    if (o.y_moved) std::copy(was, was + 3, o.y_before);
                }
            }
        }
        flip_.address = shown.address;
        flip_.width = shown.width;
        flip_.height = shown.height;
        flip_.stride = shown.stride;
        flip_.format = shown.format;
        flip_.before_slot = 1 - fill_slot_;
        flip_.final_texture = slots_[fill_slot_].texture;
        flip_.uv_max[0] = uv_max[0];
        flip_.uv_max[1] = uv_max[1];
        const auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval / factor_);
        flip_.slack = step;
        // Pictures at phases 1/k .. (k-1)/k, then the flip (phase 1); the first one goes out right now.
        for (int j = 2; j <= factor_; ++j) {
            Picture p;
            p.phase = static_cast<float>(j) / static_cast<float>(factor_);
            p.due = now + step * (j - 1);
            pictures_.push_back(p);
        }
        pictures_.push_front(Picture{1.0f / static_cast<float>(factor_), now});
        ++stats_.interpolated;
        done = true;
    }
    previous_ = std::move(mine);
    if (done) flip_.now = &previous_; // the pictures of this flip draw from it, and the next flip pairs against it
    previous_cameras_ = cameras_;
    previous_valid_ = !copied && !self_sampled && !previous_.empty();
    fill_slot_ = 1 - fill_slot_;

    static const bool log = std::getenv("MHF_INTERP_LOG") != nullptr;
    if (log && (stats_.interpolated + stats_.skipped_shape + stats_.skipped_copy + stats_.skipped_timing +
                stats_.skipped_first + stats_.skipped_ui) % 120 == 0) {
        std::fprintf(stderr,
                     "[interp] x%d: interpolated %llu, skipped: first %llu, copy %llu, timing %llu, shape %llu, "
                     "menus %llu; pictures dropped late %llu\n",
                     factor_, static_cast<unsigned long long>(stats_.interpolated),
                     static_cast<unsigned long long>(stats_.skipped_first), static_cast<unsigned long long>(stats_.skipped_copy),
                     static_cast<unsigned long long>(stats_.skipped_timing), static_cast<unsigned long long>(stats_.skipped_shape),
                     static_cast<unsigned long long>(stats_.skipped_ui), static_cast<unsigned long long>(stats_.late));
        if (cost_.pictures > 0) {
            const double n = static_cast<double>(cost_.pictures);
            std::fprintf(stderr,
                         "[interp] per in-between picture: calc %.2f ms, upload %.2f ms, state %.2f ms, draw calls %.2f ms "
                         "(%.0f batches, %.0f vertices)\n",
                         cost_.calc_us / n / 1000.0, cost_.upload_us / n / 1000.0, cost_.state_us / n / 1000.0,
                         cost_.draw_us / n / 1000.0, static_cast<double>(cost_.batches) / n,
                         static_cast<double>(cost_.vertices) / n);
            cost_ = {};
        }
    }
    return done;
}

void GlBackend::draw_overlay() {
    if (overlay_ == nullptr) return;
    {
        std::lock_guard lock(overlay_->mutex);
        if (overlay_->generation != overlay_generation_) {
            overlay_generation_ = overlay_->generation;
            if (overlay_->visible && !overlay_visible_) overlay_shown_at_ = SDL_GetTicks();
            overlay_visible_ = overlay_->visible && overlay_->width > 0;
            if (overlay_visible_) {
                gl_.BindTexture(GL_TEXTURE_2D, overlay_texture_);
                gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
                gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, overlay_->width, overlay_->height, 0, GL_RGBA,
                               GL_UNSIGNED_BYTE, overlay_->pixels.data());
            }
        }
    }
    if (!overlay_visible_) return;
    const float t = static_cast<float>(SDL_GetTicks() - overlay_shown_at_) / 150.0f;
    const float alpha = std::clamp(t, 0.0f, 1.0f);
    gl_.Disable(GL_DEPTH_TEST);
    gl_.Enable(GL_BLEND);
    gl_.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    gl_.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl_.UseProgram(overlay_program_);
    gl_.Uniform1i(overlay_tex_, 0);
    gl_.Uniform1f(overlay_alpha_, alpha * (2.0f - alpha)); // ease out
    gl_.Uniform2f(overlay_uv_max_, 1.0f, 1.0f);
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_2D, overlay_texture_);
    set_texture_params(true, true, true);
    gl_.BindVertexArray(empty_vao_);
    gl_.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_.Disable(GL_BLEND);
}

void GlBackend::dump_window(int w, int h) {
    static const char *folder = std::getenv("MHF_GL_DUMP");
    static const unsigned long every = [] {
        const char *v = std::getenv("MHF_GL_DUMP_EVERY");
        return v != nullptr ? std::max(1ul, std::strtoul(v, nullptr, 10)) : 60ul;
    }();
    static const unsigned long from = [] {
        const char *v = std::getenv("MHF_GL_DUMP_FROM"); // first presented frame to dump
        return v != nullptr ? std::strtoul(v, nullptr, 10) : 0ul;
    }();
    if (folder == nullptr || w <= 0 || h <= 0 || shown_ < from || shown_ % every != 0) return;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3u);
    gl_.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl_.ReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    char path[1024];
    std::snprintf(path, sizeof path, "%s/gl_%06llu.ppm", folder, static_cast<unsigned long long>(shown_));
    if (std::FILE *f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; --y) std::fwrite(rgb.data() + static_cast<std::size_t>(y) * w * 3u, 1, static_cast<std::size_t>(w) * 3u, f);
        std::fclose(f);
    }
}

} // namespace

std::unique_ptr<gpu::Backend> make_gl_backend(SDL_Window *window, int scale, psprecomp::GuestMemory &memory,
                                              bool smooth_textures, OverlayImage *overlay, int target_fps) {
    if (const char *v = std::getenv("MHF_FPS")) target_fps = std::atoi(v); // testing aid
    // The game flips at 30 fps: the output rate is a whole multiple of it, from 1x (as the game) to 6x (180 fps).
    const int factor = std::clamp((target_fps + 15) / 30, 1, 6);
    auto backend = std::make_unique<GlBackend>(window, scale, memory, smooth_textures, overlay, factor);
    if (!backend->init()) return nullptr;
    return backend;
}

} // namespace mhf::psp
