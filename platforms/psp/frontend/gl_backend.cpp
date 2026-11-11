// OpenGL 3.3 drawing for the GE. See gl_backend.hpp and docs/05-runtime-hle.md §6.
//
// Model: every framebuffer address the game draws to gets a render target (color texture + depth buffer) at
// `scale` times the PSP resolution. Primitives arrive in screen space from the Ge; consecutive ones with the same
// draw state are batched into one draw call. Textures are decoded once and cached by content hash; a texture that
// lives inside a render target is sampled from that target directly (render-to-texture effects). Block transfers out
// of a render target are done on the GPU. The displayed framebuffer is drawn to the window, letterboxed, when the
// game flips; one the GE has not drawn (CPU-made screens, movies) is uploaded from guest memory instead.

#include "gl_backend.hpp"
#include "frame_matching.hpp"
#include "texture_images.hpp"
#include "texture_pack.hpp"

#include "ge.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
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
#include <cctype>
#include <cstring>
#include <unordered_set>
#include <string>
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
using MhfPolygonOffset = void(APIENTRY *)(GLfloat, GLfloat);
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
    X(PFNGLGENQUERIESPROC, GenQueries, glGenQueries)                                                               \
    X(PFNGLBEGINQUERYPROC, BeginQuery, glBeginQuery)                                                               \
    X(PFNGLENDQUERYPROC, EndQuery, glEndQuery)                                                                     \
    X(PFNGLGETQUERYOBJECTUIVPROC, GetQueryObjectuiv, glGetQueryObjectuiv)                                          \
    X(PFNGLGETQUERYOBJECTUI64VPROC, GetQueryObjectui64v, glGetQueryObjectui64v)                                    \
    X(MhfViewport, Viewport, glViewport)                                                                     \
    X(MhfScissor, Scissor, glScissor)                                                                        \
    X(MhfEnable, Enable, glEnable)                                                                           \
    X(MhfDisable, Disable, glDisable)                                                                        \
    X(MhfClear, Clear, glClear)                                                                              \
    X(MhfClearColor, ClearColor, glClearColor)                                                               \
    X(MhfColorMask, ColorMask, glColorMask)                                                                  \
    X(MhfDepthMask, DepthMask, glDepthMask)                                                                  \
    X(MhfPolygonOffset, PolygonOffset, glPolygonOffset)                                                      \
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
    X(PFNGLGENERATEMIPMAPPROC, GenerateMipmap, glGenerateMipmap)                                                   \
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
    X(PFNGLMAPBUFFERRANGEPROC, MapBufferRange, glMapBufferRange)                                                   \
    X(PFNGLUNMAPBUFFERPROC, UnmapBuffer, glUnmapBuffer)                                                            \
    X(PFNGLFENCESYNCPROC, FenceSync, glFenceSync)                                                                  \
    X(PFNGLCLIENTWAITSYNCPROC, ClientWaitSync, glClientWaitSync)                                                   \
    X(PFNGLDELETESYNCPROC, DeleteSync, glDeleteSync)                                                               \
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

// ---------------------------------------------------------------- projected shadows (MHF_SHADOW=1)
//
// With the whole map on (LiveSettings::shadow_whole_map) everything solid casts, and the surfaces the camera sees that turn
// away from the sun are darkened from their real triangles. Without it only objects cast: what is skinned (the hunter, the
// monsters) and what the game places with its own matrix and is small enough. Everything happens in eye space, because the
// game keeps its camera in the world matrix: there is no world frame to recover, and the game's light is already there.
// Run once per picture, between the 3D scene and the 2D drawing over it.

constexpr const char *kShadowMapVertex = R"(#version 330 core
layout(location = 0) in vec3 a_eye;
layout(location = 1) in vec2 a_uv;
uniform vec3 u_center, u_right, u_up, u_dir; // the sun's box in eye space; u_dir points to the sun
uniform vec3 u_half;
out vec2 v_uv;
void main() {
    vec3 d = a_eye - u_center;
    gl_Position = vec4(dot(d, u_right) / u_half.x, dot(d, u_up) / u_half.y, -dot(d, u_dir) / u_half.z, 1.0);
    v_uv = a_uv;
}
)";

// Cut-outs keep the shape of their leaves: the texels the game's alpha test throws away are thrown away here too.
constexpr const char *kShadowMapFragment = R"(#version 330 core
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec4 u_uv_xform;
uniform int u_alpha_func, u_alpha_ref, u_alpha_mask; // u_alpha_func 1: nothing is thrown away
uniform int u_clamp_s, u_clamp_t;
uniform float u_scenery;                             // 1: scenery, whose shadows have soft edges even close to it
uniform float u_push_limit;                          // the most a surface is pushed away from the sun, in depth
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
    if (u_alpha_func != 1) {
        ivec2 size = textureSize(u_tex, 0);
        ivec2 t = ivec2(floor((v_uv * u_uv_xform.xy + u_uv_xform.zw) * vec2(size)));
        t.x = u_clamp_s != 0 ? clamp(t.x, 0, size.x - 1) : ((t.x % size.x) + size.x) % size.x;
        t.y = u_clamp_t != 0 ? clamp(t.y, 0, size.y - 1) : ((t.y % size.y) + size.y) % size.y;
        int a = int(clamp(texelFetch(u_tex, t, 0).a, 0.0, 1.0) * 255.0 + 0.5) & u_alpha_mask;
        if (!compare(u_alpha_func, a, u_alpha_ref & u_alpha_mask)) discard;
    }
    // Pushed away from the sun by its slope in the map, so that the scenery does not shadow itself. With a limit: a wall almost
    // along the rays would go so far back that sun leaks onto the ledges at its foot.
    float slope = max(abs(dFdx(gl_FragCoord.z)), abs(dFdy(gl_FragCoord.z)));
    gl_FragDepth = gl_FragCoord.z + min(3.0 * slope, u_push_limit);
    o_color = vec4(u_scenery);
}
)";

constexpr const char *kShadowQuadVertex = R"(#version 330 core
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// For every pixel of the scene, how much light is left. The depth is the GE's screen depth (0..1 for z 0..65535): the scene's
// projection and viewport turn it back into the eye space position, as the Ge computed it.
constexpr const char *kShadowFactorFragment = R"(#version 330 core
uniform sampler2D u_depth, u_map, u_kind, u_player_map;
uniform vec3 u_player_center;                        // the player's own small map, with the sun's box of the same axes
uniform float u_player_half;                         // half its side: 0 when there is none
uniform vec2 u_size;                                 // the target in pixels
uniform vec4 u_proj;                                 // projection elements 0, 5, 10, 11
uniform vec2 u_proj_z;                               // elements 14, 15
uniform vec2 u_vz;                                   // viewport depth scale and centre
uniform vec4 u_vp;                                   // viewport: x and y scale, x and y centre minus the screen offset
uniform float u_pixel;                               // pixels of the target per PSP pixel
uniform vec3 u_center, u_right, u_up, u_dir, u_half; // the sun's box, as in kShadowMapVertex
uniform vec3 u_rc0, u_rc1, u_rc2, u_t;               // this picture's eye space to the map's (in-between pictures)
uniform float u_darkness, u_bias, u_drop, u_soft;    // u_drop: how far below a caster its shadow still reaches
uniform vec3 u_fog;                                  // the game's fog: end, scale, and how hard the shadows fade in it (0: not)
uniform int u_down;                                  // pixels of the picture per pixel of the factor, each way
out vec4 o_color;
float eye_z(float d) {
    float zn = (d * 65535.0 - u_vz.y) / u_vz.x;
    return (u_proj_z.x - zn * u_proj_z.y) / (zn * u_proj.w - u_proj.z);
}
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
// The player's own shadow here (0..1), from its small map.
float player_shadow(vec3 v, float bias_world, float turn) {
    if (u_player_half <= 0.0) return 0.0;
    vec3 from = v + u_center - u_player_center;
    vec3 l = vec3(dot(from, u_right), dot(from, u_up), -dot(from, u_dir) / 3.0) / u_player_half;
    if (max(abs(l.x), abs(l.y)) >= 1.0) return 0.0;
    float span = 6.0 * u_player_half, texel = 1.0 / float(textureSize(u_player_map, 0).x);
    vec2 uv = l.xy * 0.5 + 0.5;
    float here = min(l.z * 0.5 + 0.5, 1.0), bias = bias_world / span, shade = 0.0;
    const int N = 12;
    for (int i = 0; i < N; ++i) {
        float a = turn + float(i) * 2.399963, r = 2.5 * sqrt((float(i) + 0.5) / float(N));
        if (here - texture(u_player_map, uv + vec2(cos(a), sin(a)) * r * texel).r > bias) shade += 1.0;
    }
    return shade / float(N);
}
void main() {
    o_color = vec4(1.0, 1.0, 1.0, 0.0); // alpha: how much of the shadow here is the scenery's (only that is blurred afterwards)
    ivec2 ip = ivec2(gl_FragCoord.xy) * u_down;
    float d = texelFetch(u_depth, ip, 0).r;
    if (d >= 0.99995) return; // the sky
    float ze = eye_z(d);
    // In a misty zone the fog hides what is far: its shadows fade with it, as the game fades the colours.
    float dark = u_darkness * (u_fog.z > 0.0 ? pow(clamp((ze + u_fog.x) * u_fog.y, 0.0, 1.0), u_fog.z) : 1.0);
    float w = u_proj.w * ze + u_proj_z.y;
    vec2 ndc = ((vec2(ip) + 0.5) / u_pixel - u_vp.zw) / u_vp.xy;
    vec3 x = vec3(ndc.x * w / u_proj.x, ndc.y * w / u_proj.y, ze) - u_t;
    vec3 v = vec3(dot(u_rc0, x), dot(u_rc1, x), dot(u_rc2, x)) - u_center;
    vec3 l = vec3(dot(v, u_right) / u_half.x, dot(v, u_up) / u_half.y, -dot(v, u_dir) / u_half.z);
    float edge = max(abs(l.x), abs(l.y));
    float step_size = abs(eye_z(d + 1.0 / 65535.0) - ze); // one step of the GE's depth, as a distance
    float turn = ign(gl_FragCoord.xy) * 6.2831853;
    float player = player_shadow(v, u_bias + 1.5 * step_size, turn);
    o_color = vec4(vec3(1.0 - dark * player), 0.0);
    if (edge >= 1.0 || abs(l.z) >= 1.0) return;
    vec2 uv = l.xy * 0.5 + 0.5;
    float here = l.z * 0.5 + 0.5, span = 2.0 * u_half.z;
    float bias = (u_bias + 1.5 * step_size) / span;
    float texel = 1.0 / float(textureSize(u_map, 0).x);
    // The radii below are in texels of the biggest map: a smaller one keeps the same softness, with its own coarser texels.
    float fine = float(textureSize(u_map, 0).x) / 8192.0;
    bias = (u_bias / sqrt(fine) + 1.5 * step_size) / span;
    float shade = 0.0;
    // The farther the caster, the softer the edge: sharp at the feet, blurred for a ledge high up, as under the real sun. Low
    // polygons cast straight edges, which this hides.
    float blockers = 0.0, found = 0.0, scenery = 0.0;
    for (int i = 0; i < 8; ++i) {
        // Most of the looks stay near the middle: spread evenly, they would step over what is thin, a trunk or a cat, and it would
        // cast nothing on most pixels.
        float a = turn + float(i) * 2.399963, q = (float(i) + 0.5) / 8.0, r = 24.0 * fine * q * q;
        vec2 at = uv + vec2(cos(a), sin(a)) * r * texel;
        float gap = here - texture(u_map, at).r;
        if (gap > bias) { blockers += gap * span; found += 1.0; scenery += texture(u_kind, at).r; }
    }
    if (found == 0.0) return;
    // The low polygons of the scenery cast straight edges, also on themselves, right next to what casts them.
    float share = scenery / found; // characters and objects keep their shadows sharper
    float radius = min(u_soft + blockers / found * mix(0.005, 0.03, share) / (2.0 * u_half.x * texel), 32.0 * fine);
    radius = max(radius, 16.0 * fine * share);
    const int N = 16;
    for (int i = 0; i < N; ++i) {
        float a = turn + float(i) * 2.399963, r = radius * sqrt((float(i) + 0.5) / float(N));
        float gap = here - texture(u_map, uv + vec2(cos(a), sin(a)) * r * texel).r;
        if (gap > bias) shade += 1.0 - smoothstep(0.6 * u_drop, u_drop, gap * span);
    }
    shade *= (1.0 - smoothstep(0.9, 1.0, edge)) / float(N);
    o_color = vec4(vec3(1.0 - dark * max(shade, player)), share);
}
)";

// Letters for the zone label: five bits a row, seven rows.
constexpr const char *kShadowTestBuild = "V299";
// The jungle camp's waterfall and mountain rules (V242 to V244).
constexpr bool kJungleCampWater = true;
constexpr struct {
    char letter;
    std::uint8_t rows[7];
} kLabelFont[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}},
    {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 14}},
    {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {14, 4, 4, 4, 4, 4, 14}},
    {'J', {7, 2, 2, 2, 2, 18, 12}},
    {'K', {17, 18, 20, 24, 20, 18, 17}},
    {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}},
    {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'Q', {14, 17, 17, 17, 21, 18, 13}},
    {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},
    {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}},
    {'W', {17, 17, 17, 21, 21, 27, 17}},
    {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},
    {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {30, 1, 1, 14, 1, 1, 30}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},
    {'5', {31, 16, 30, 1, 1, 17, 14}},
    {'6', {6, 8, 16, 30, 17, 17, 14}},
    {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 2, 12}},
};

// With a small blur: the wide soft edges come from a few samples, which would show as grain.
constexpr const char *kShadowApplyFragment = R"(#version 330 core
uniform sampler2D u_factor, u_depth;
uniform int u_step, u_down; // u_down: pixels of the picture per pixel of the factor, each way
out vec4 o_color;
void main() {
    ivec2 full = ivec2(gl_FragCoord.xy), ip = full / u_down, size = textureSize(u_factor, 0) - ivec2(1);
    vec4 sum = vec4(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) sum += texelFetch(u_factor, clamp(ip + ivec2(x, y) * u_step, ivec2(0), size), 0);
    }
    sum /= 9.0;
    vec3 sharp = texelFetch(u_factor, ip, 0).rgb;
    if (u_down > 1) {
        // Brought up by the nearest pixels of the factor at the same depth: a shadow must not spill over the edge of what stands
        // in front of it, as a plain blend would.
        float here = texelFetch(u_depth, full, 0).r;
        vec2 at = (vec2(full) + 0.5) / float(u_down) - 0.5;
        ivec2 corner = ivec2(floor(at));
        vec2 f = at - vec2(corner);
        vec3 total = vec3(0.0);
        float weight = 0.0, nearest = 1e9;
        vec3 closest = sharp;
        for (int y = 0; y <= 1; ++y) {
            for (int x = 0; x <= 1; ++x) {
                ivec2 k = clamp(corner + ivec2(x, y), ivec2(0), size);
                float gap = abs(texelFetch(u_depth, k * u_down, 0).r - here);
                float w = (x == 1 ? f.x : 1.0 - f.x) * (y == 1 ? f.y : 1.0 - f.y) * exp(-gap * 4000.0) + 1e-5;
                vec3 c = texelFetch(u_factor, k, 0).rgb;
                total += c * w;
                weight += w;
                if (gap < nearest) { nearest = gap; closest = c; }
            }
        }
        sharp = weight > 0.02 ? total / weight : closest;
    }
    // Only the scenery's wide edges have grain: the shadows of characters and objects stay as they are.
    o_color = vec4(mix(sharp, sum.rgb, clamp(sum.a * 4.0, 0.0, 1.0)), 1.0);
}
)";

// The surfaces turned away from the sun, drawn over the factor against the scene's depth. Vertices are in the eye space of
// the map's picture; an in-between picture moves them to its own.
constexpr const char *kShadowFacingVertex = R"(#version 330 core
layout(location = 0) in vec3 a_eye;
layout(location = 1) in float a_reach;
layout(location = 2) in float a_depth; // what to add to the depth worked out here to get the one the game drew it with
uniform vec4 u_proj, u_vp;
uniform vec2 u_proj_z, u_vz, u_target;
uniform vec3 u_rc0, u_rc1, u_rc2, u_t; // this picture's eye space to the map's, as in kShadowFactorFragment
uniform vec3 u_f0, u_f1, u_f2, u_ft;  // the eye space these triangles were worked out in to the map's: they last a few flips
uniform int u_clear;                  // 1: taking the shadows off pieces of the air, at their exact depth
uniform vec3 u_fog;                   // as in kShadowFactorFragment
out float v_reach, v_fog;
void main() {
    vec3 m = u_f0 * a_eye.x + u_f1 * a_eye.y + u_f2 * a_eye.z + u_ft;
    vec3 e = vec3(dot(m, vec3(u_rc0.x, u_rc1.x, u_rc2.x)), dot(m, vec3(u_rc0.y, u_rc1.y, u_rc2.y)),
             dot(m, vec3(u_rc0.z, u_rc1.z, u_rc2.z))) + u_t;
    float w = u_proj.w * e.z + u_proj_z.y;
    vec2 pixel = e.xy * u_proj.xy / w * u_vp.xy + u_vp.zw;
    // Layers of ground lie a little over the surface shaded here, and would cut the shade along their edges: it is drawn a little
    // nearer to the camera, along the line of sight, which leaves it in the same place on the screen.
    float e_length = max(length(e), 1.0);
    float ze = u_clear != 0 ? e.z : e.z * max(1.0 - 10.0 / e_length, 0.0);
    float z = ((u_proj.z * ze + u_proj_z.x) / (u_proj.w * ze + u_proj_z.y) * u_vz.x + u_vz.y) / 65535.0 + a_depth;
    gl_Position = vec4((pixel / u_target * 2.0 - 1.0) * w, (z * 2.0 - 1.0) * w, w);
    v_reach = a_reach;
    v_fog = u_fog.z > 0.0 ? pow(clamp((e.z + u_fog.x) * u_fog.y, 0.0, 1.0), u_fog.z) : 1.0;
}
)";
constexpr const char *kShadowFacingFragment = R"(#version 330 core
in float v_reach, v_fog;
uniform float u_darkness;
uniform int u_clear, u_down;
uniform sampler2D u_depth;
out vec4 o_color;
void main() {
    float scene = texelFetch(u_depth, ivec2(gl_FragCoord.xy) * u_down, 0).r;
    // Only where the scene's depth is this very piece: not what stands before it, nor what shows through its gaps.
    if (u_clear != 0 && abs(scene - gl_FragCoord.z) > 0.0004) discard;
    // The rest against the scene's depth, with a little to spare on slopes.
    float spare = max(abs(dFdx(gl_FragCoord.z)), abs(dFdy(gl_FragCoord.z))) + 8.0 / 16777216.0;
    if (u_clear == 0 && gl_FragCoord.z > scene + spare) discard;
    o_color = vec4(vec3(1.0 - u_darkness * v_fog * (1.0 - v_reach)), 1.0);
}
)";

// The "show what casts" view: every object of the picture in the colour of its class, over the scene.
constexpr const char *kShadowShowVertex = R"(#version 330 core
layout(location = 0) in vec3 a_eye;
uniform vec4 u_proj, u_vp;
uniform vec2 u_proj_z, u_target;
void main() {
    float w = u_proj.w * a_eye.z + u_proj_z.y;
    vec2 ndc = a_eye.xy * u_proj.xy;
    vec2 pixel = ndc / w * u_vp.xy + u_vp.zw;
    gl_Position = vec4((pixel / u_target * 2.0 - 1.0) * w, 0.0, w);
}
)";
constexpr const char *kShadowShowFragment = R"(#version 330 core
uniform vec4 u_color;
out vec4 o_color;
void main() { o_color = u_color; }
)";

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

// The blurred background of the in-game menu: a picture drawn into a small target and blurred there. The vertex shader
// does not flip (the targets are read back the way they were written) and maps the quad onto a part of the texture.
constexpr const char *kBlurVertex = R"(#version 330 core
uniform vec2 u_uv_off;
uniform vec2 u_uv_scale;
out vec2 v_uv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv = u_uv_off + p * u_uv_scale;
}
)";

// Five taps that stand for nine of a Gaussian (the two outer ones sample between texels), darkened by u_dim.
constexpr const char *kBlurFragment = R"(#version 330 core
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_dir;
uniform float u_dim;
out vec4 o_color;
void main() {
    vec3 c = texture(u_tex, v_uv).rgb * 0.2270270270;
    c += texture(u_tex, v_uv + u_dir * 1.3846153846).rgb * 0.3162162162;
    c += texture(u_tex, v_uv - u_dir * 1.3846153846).rgb * 0.3162162162;
    c += texture(u_tex, v_uv + u_dir * 3.2307692308).rgb * 0.0702702703;
    c += texture(u_tex, v_uv - u_dir * 3.2307692308).rgb * 0.0702702703;
    o_color = vec4(c * u_dim, 1.0);
}
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
// Where each register sits in a DrawKey (-1: not a draw-state register).
constexpr std::array<int, 256> kKeyIndex = [] {
    std::array<int, 256> index{};
    index.fill(-1);
    for (std::size_t i = 0; i < kStateRegs.size(); ++i) index[kStateRegs[i] & 0xFFu] = static_cast<int>(i);
    return index;
}();

std::uint32_t key_reg(const DrawKey &key, std::uint32_t reg) {
    if (reg == kMaskAlpha) return key[kStateRegs.size() + 1];
    const int i = kKeyIndex[reg & 0xFFu];
    return i >= 0 ? key[static_cast<std::size_t>(i)] : 0; // not a draw-state register: nothing here reads one
}

// Everything needed to set up OpenGL for a batch of primitives: the registers plus what was resolved from guest
// memory when the batch started (the texture and the render target).
struct DrawSetup {
    DrawKey key{};
    GLuint texture{};     // 0: untextured
    float uv_xform[4]{1, 1, 0, 0};
    bool texture_mips{};          // the texture has mip levels (a texture pack image)
    std::uint8_t texture_filter{}; // a texture pack's [filtering] for it: 0 the game's, 1 nearest, 2 linear
    bool through{};
    float tex_width{1}, tex_height{1};
    std::uint32_t target{}; // VRAM offset of the render target
    bool self_sampled{};    // the texture is a copy of the target being drawn: it cannot be replayed later
    bool haze{};            // haze on the game's list: the projected shadows must come before it
    std::uint64_t tex_hash{}; // the texture's, for the shadows
    bool listed{};            // on one of the shadows' lists of textures
    std::uint32_t largest_draw{}; // triangles of the largest single draw in the batch
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

// Where a register sits in a DrawKey.
constexpr std::size_t state_index(std::uint32_t reg) {
    for (std::size_t i = 0; i < kStateRegs.size(); ++i) {
        if (kStateRegs[i] == reg) return i;
    }
    return kStateRegs.size();
}

// A see-through sheet hanging right in front of the camera. It writes depth, which would hide the scene from the shadows, and
// the game needs that depth: it keeps what comes next from showing. So the shadows are worked out before it.
bool is_camera_haze(const DrawKey &key, const std::vector<GpuVertex> &vertices) {
    if ((key_reg(key, kAlphaBlendEnable) & 1u) == 0 || (key_reg(key, kZTestEnable) & 1u) == 0 || (key_reg(key, kZWriteDisable) & 1u) != 0) return false;
    // One alpha for the whole sheet: layers of ground fade from vertex to vertex.
    return !vertices.empty() && std::all_of(vertices.begin(), vertices.end(), [&](const GpuVertex &v) {
        return v.color[3] < 200u && v.color[3] == vertices.front().color[3] && v.inv_w > 1.0f / 60.0f;
    });
}

// Effects: blended and not writing depth (particles such as dust and smoke, but also layers such as the snow over the
// terrain).
bool is_effect(const DrawKey &key) {
    return (key[state_index(kAlphaBlendEnable)] & 1u) != 0 && (key[state_index(kZWriteDisable)] & 1u) != 0;
}

// A particle system reuses the memory of a particle that ended for a new one somewhere else, and the points are paired
// by that memory: moving them back drew the new particle on its way from the old one (fragments around the footsteps at
// 180 fps). A triangle of a reused particle jumps several times its own size in one flip, which the triangles of a layer
// such as the snow over the terrain never do (measured: none in a run through the village), although their points shift
// a little every flip and need their pairing. So in an effect, a piece (triangles joined by shared points) in which any
// fully paired triangle jumped more than twice its own size is drawn where this flip put it, through the interpolated
// camera, as a whole so that no shared point is torn apart. Everything else keeps its pairing.
void unpair_jumping_particles(std::vector<Origin> &origins) {
    for (const Origin &o : origins) {
        if (o.sprite) return; // triangles only (sprites take x and y from different points)
    }
    const std::size_t count = origins.size() / 3;
    if (count == 0) return;
    const auto distance = [](const float *a, const float *b) {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    // Triangles that jumped.
    std::vector<bool> jumped(count, false);
    bool any = false;
    for (std::size_t t = 0; t < count; ++t) {
        const Origin *p = &origins[t * 3];
        if (!p[0].x_moved || !p[1].x_moved || !p[2].x_moved) continue;
        const float size = std::max({distance(p[0].x_world, p[1].x_world), distance(p[1].x_world, p[2].x_world),
                                     distance(p[0].x_world, p[2].x_world)});
        const float jump = std::max({distance(p[0].x_world, p[0].x_before), distance(p[1].x_world, p[1].x_before),
                                     distance(p[2].x_world, p[2].x_before)});
        jumped[t] = jump > 2.0f * size;
        any = any || jumped[t];
    }
    if (!any) return;
    // Pieces: triangles that share a point (the same key) are joined.
    std::vector<std::size_t> parent(count);
    for (std::size_t i = 0; i < count; ++i) parent[i] = i;
    const auto root = [&](std::size_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    std::unordered_map<std::uint64_t, std::size_t> owner;
    owner.reserve(origins.size());
    for (std::size_t i = 0; i < origins.size(); ++i) {
        if (origins[i].x_key == 0) continue;
        const auto [it, fresh] = owner.emplace(origins[i].x_key, i / 3);
        if (!fresh) parent[root(i / 3)] = root(it->second);
    }
    std::vector<bool> piece_jumped(count, false);
    for (std::size_t t = 0; t < count; ++t) {
        if (jumped[t]) piece_jumped[root(t)] = true;
    }
    for (std::size_t t = 0; t < count; ++t) {
        if (!piece_jumped[root(t)]) continue;
        for (std::size_t a = t * 3; a < t * 3 + 3; ++a) origins[a].x_moved = origins[a].y_moved = false;
    }
}

// Interpolation v2: a 3D triangle as the GE had it before clipping, to be moved, clipped and placed again for each
// in-between picture: its points in the world (after the world matrix), what the vertices carry, and its draw.
struct RawVertex {
    float world[3];
    float u, v;
    std::uint8_t color[4], spec[4];
    float fog;
};
struct RawTriangle {
    RawVertex v[3];
    std::uint32_t draw; // index into the flip's draw summaries
};

// One batch of primitives as it was drawn, kept to draw it again between two flips (frame interpolation).
struct TapeEntry {
    DrawSetup setup;
    std::vector<GpuVertex> vertices;
    std::vector<Origin> origins; // one per vertex
    std::vector<RawTriangle> raw; // interpolation v2: the batch's 3D triangles before clipping
    bool raw_complete = true;     // every primitive of the batch is among them (no 3D sprite, no point without a world)
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
              OverlayImage *overlay, int factor, LiveSettings *live)
        : window_(window), scale_(std::max(1, scale)), memory_(memory), live_(live != nullptr ? live : &own_live_),
          overlay_(overlay), factor_(std::clamp(factor, 1, 6)), interpolate_(factor > 1) {
        if (live == nullptr) own_live_.smooth_textures = smooth_textures;
        open_texture_pack();
    }

    ~GlBackend() override {
        if (context_ != nullptr) SDL_GL_DestroyContext(context_);
    }

    bool init();

    // The game's own round shadows under hunters and monsters: over the ground without writing the depth, from a texture each
    // map shares with dust and other effects. Those are light and fade; these are always the same dark grey and opaque, and
    // nothing else that dark was drawn with this alpha test.
    bool hidden_round_shadow(const gpu::Ge &ge, const gpu::Vertex &a) {
        if ((ge.reg(kZWriteDisable) & 1u) == 0 || ge.reg(kBlendMode) != 0x32u || a.a < 250.0f || ge.reg(kAlphaTest) != 0x00FF0006u) return false;
        const auto dark_grey = [](float value) { return value >= 15.0f && value <= 35.0f; };
        if (!dark_grey(a.r) || !dark_grey(a.g) || !dark_grey(a.b)) return false;
        return live_ != nullptr && live_->shadow_hide_round.load(std::memory_order_relaxed);
    }

    void triangle(const gpu::Ge &ge, const gpu::Vertex &a, const gpu::Vertex &b, const gpu::Vertex &c) override {
        if (hidden_round_shadow(ge, a)) return;
        prepare(ge);
        if (!through_ && !(a.has_world && b.has_world && c.has_world)) raw_incomplete(); // a 3D line, say
        const bool flat = (ge.reg(kShadeMode) & 1u) == 0;
        push(a, flat ? &c : nullptr);
        push(b, flat ? &c : nullptr);
        push(c, nullptr);
        setup_.largest_draw = std::max(setup_.largest_draw, ++draw_triangles_);
        if (needs_origins()) {
            const std::uint16_t camera = a.has_world && b.has_world && c.has_world ? camera_for(ge) : kNoCamera;
            record(a, a, camera, false);
            record(b, b, camera, false);
            record(c, c, camera, false);
        }
    }

    void draw_started(const gpu::DrawInfo &draw) override {
        ++draw_serial_; // the GE's state can only have changed since the last draw
        draw_triangles_ = 0;
        shadow_begin_draw(draw);
        if (!interpolate_) return;
        draw_use_ = ++draw_uses_[draw.vertex_address];
        recording_.push_back(interpolation::summarize(draw));
        recording_viewports_.push_back(draw.viewport);
    }

    void triangle_unclipped(const gpu::Ge &ge, const gpu::Vertex &a, const gpu::Vertex &b, const gpu::Vertex &c) override {
        if (hidden_round_shadow(ge, a)) return;
        if (shadow_draw_ok_) {
            // One triangle in 32 is timed: asking the clock for each would cost more than some of them.
            if ((++shadow_capture_seen_ & 31u) == 0u) {
                const auto from = std::chrono::steady_clock::now();
                shadow_capture(ge, a, b, c);
                shadow_capture_ms_ += 32.0 * std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
            } else {
                shadow_capture(ge, a, b, c);
            }
        }
        if (!interpolate_ || !v2_enabled_ || recording_.empty()) return;
        prepare(ge);
        if (!a.has_world || !b.has_world || !c.has_world) {
            raw_incomplete();
            return;
        }
        const bool flat = (ge.reg(kShadeMode) & 1u) == 0;
        RawTriangle t;
        t.draw = static_cast<std::uint32_t>(recording_.size() - 1);
        const gpu::Vertex *in[3] = {&a, &b, &c};
        for (int i = 0; i < 3; ++i) {
            const gpu::Vertex &v = *in[i];
            const gpu::Vertex &col = flat ? c : v; // flat shading: the last vertex's colour
            RawVertex &r = t.v[i];
            std::copy(v.world, v.world + 3, r.world);
            r.u = v.u;
            r.v = v.v;
            r.color[0] = to_byte(col.r); r.color[1] = to_byte(col.g); r.color[2] = to_byte(col.b); r.color[3] = to_byte(col.a);
            r.spec[0] = to_byte(col.sr); r.spec[1] = to_byte(col.sg); r.spec[2] = to_byte(col.sb); r.spec[3] = 0;
            r.fog = v.fog;
        }
        raw_batch_.push_back(t);
    }

    void sprite(const gpu::Ge &ge, const gpu::Vertex &a, const gpu::Vertex &b) override {
        prepare(ge);
        if (!through_) raw_incomplete(); // a 3D sprite: interpolation v2 cannot make this batch again
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
        draw_triangles_ += 2;
        setup_.largest_draw = std::max(setup_.largest_draw, draw_triangles_);
        if (needs_origins()) {
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
        if (pacing_.active) return pacing_.next_due;
        if (overlay_ != nullptr) { // the menu is up: redraw it every refresh, even when the game shows nothing new
            std::lock_guard lock(overlay_->mutex);
            if (overlay_->continuous) return last_show_ + std::chrono::milliseconds(16);
        }
        return std::nullopt;
    }
    void tick() override;

private:
    struct RenderTarget;
    // Projected shadows: the 3D of the picture by object, in eye space.
    struct ShadowGroup { // the draws of one picture that share their matrices: one object
        std::array<float, 12> world{}, view{};
        bool skinned{};
        float axis[3][3]{};                      // the world matrix's axes, unscaled, to measure the object in its own frame
        float low[3]{1e30f, 1e30f, 1e30f}, high[3]{-1e30f, -1e30f, -1e30f}; // its bounds in that frame
        float eye_low[3]{1e30f, 1e30f, 1e30f}, eye_high[3]{-1e30f, -1e30f, -1e30f};
        std::uint32_t triangles{};
        std::uint64_t place{};                   // how far it stands from the picture's first object, which the camera does not change
        [[nodiscard]] float size() const {
            return std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2]});
        }
    };
    struct ShadowDraw {
        std::uint64_t known_key{};
        std::uint32_t group{}, first{}, count{}, target{};
        GLuint texture{};
        float xform[4]{1, 1, 0, 0};
        std::int32_t func{1}, ref{}, mask{255}, clamp_s{}, clamp_t{};
        bool cutout{};                           // a texture with real holes: leaves, fronds, grass
        bool blended{};                          // layers over the ground, haze: no whole-map shadows from these
        bool translucent{};                      // blended with a texture that fades: clouds, haze, dust
        bool has_holes{};                        // its texture has some clear texels
        std::uint64_t texture_hash{};            // which of the game's textures it uses (0: none)
        int zone{-1};                            // the zone of its pack image
        std::uint32_t vertices{};                // where the game keeps its vertices: the same from one flip to the next
        std::uint32_t fog_color{};
        float fog_low{1e30f}, fog_high{-1e30f}; // how much of its own colour the fog leaves: 1 all, 0 none
        std::uint32_t paint[7]{};                // how it paints, for the test views: blend, its two fixed colours, colour mask, alpha test, texture function, material
        std::uint8_t listed{};                   // the game's list says what its texture is: 1 air, 2 solid, 3 inside of a cave
        float holes{-1.0f}, alpha_low{255.0f}, alpha_high{0.0f}; // the texture's clear share; the vertices' alpha
        float low[3]{1e30f, 1e30f, 1e30f}, high[3]{-1e30f, -1e30f, -1e30f}; // its bounds in the object's frame
        [[nodiscard]] float size() const { return std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2]}); }
        [[nodiscard]] float height() const { return high[1] - low[1]; }
        [[nodiscard]] float width() const { return std::max(high[0] - low[0], high[2] - low[2]); }
    };
    struct ShadowRange { // what of the picture goes into the map
        std::uint32_t draw{}, first{}, count{};
    };
    // Everything a picture's shadows need to be worked out again from its depth (an in-between picture).
    struct ShadowPass {
        bool valid{};
        std::uint64_t generation{};
        std::uint32_t address{};
        int map{};
        float proj[6]{}, vz[2]{}, vp[4]{};
        float center[3]{}, right[3]{}, up[3]{}, dir[3]{};
        float half{}, darkness{}, drop{};
        float fog[3]{};                          // the game's fog end and scale, and 1 where the shadows fade in it
        float player_center[3]{}, player_half{}; // the player's own map (half 0: none)
        int facing{};                            // which buffer holds the surfaces turned away from the sun
        float facing_motion[12]{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}; // from the eye space they were worked out in to this one's
    };
    struct KeptRange { // one run of triangles of a map, with what it takes to draw it again
        std::uint32_t first{}, count{}, vertices{};
        GLuint texture{};
        float xform[4]{1, 1, 0, 0};
        std::int32_t func{1}, ref{}, mask{255}, clamp_s{}, clamp_t{};
        bool scenery{}, player{};
        bool fixed{}; // part of the scenery: it can be kept from one picture to the next
        mutable bool paired{};
    };
    struct ShadowKept {
        std::uint64_t generation{};
        std::vector<float> vertices; // eye x, y, z and u, v, as in shadow_vertices_
        std::vector<KeptRange> ranges;
    };
    // `fbo`: where to, when not the map's own. `moving_only`: over what the map already holds, without what is kept.
    void draw_shadow_map(const ShadowPass &p, const ShadowKept &kept, const std::vector<float> &vertices, bool player, GLuint fbo = 0,
                         bool moving_only = false);
    void shadow_forget_scenery();
    void shadow_begin_draw(const gpu::DrawInfo &draw);
    void shadow_capture(const gpu::Ge &ge, const gpu::Vertex &a, const gpu::Vertex &b, const gpu::Vertex &c);
    bool shadow_setup();
    void run_shadow(RenderTarget &rt);
    void shadow_time_begin(int slot);
    void shadow_time_end();
    void apply_shadow(RenderTarget &rt, const ShadowPass &p, const float *eye_to_map);
    void apply_live_scale();
    void apply_live_fps();
    void build_blur(GLuint source, float uv_max_x, float uv_max_y);
    void draw_blurred_background(int window_w, int window_h);
    struct RenderTarget {
        std::uint32_t address{}; // VRAM offset
        std::uint32_t stride{};  // pixels per line
        std::uint32_t width{}, height{};
        std::uint32_t format{};
        GLuint fbo{}, color{}, depth{};
        std::uint64_t last_draw{}; // present count when last drawn by the GE
        std::uint64_t last_use{};
        std::uint64_t copied{}; // present count + 1 when last copied back to VRAM (copy_shown_to_vram), 0: never
        [[nodiscard]] std::uint32_t bytes_per_pixel() const { return format == 3 ? 4u : 2u; }
        [[nodiscard]] std::uint32_t end() const { return address + stride * height * bytes_per_pixel(); }
    };
    struct CachedTexture {
        GLuint texture{};
        std::uint64_t hash{};
        std::uint64_t checked_frame{~0ull};
        std::uint64_t checked_clut{~0ull};
        std::uint64_t last_use{};
        // Texture pack: the image drawn instead of the game's (`shown`: its texture is then pack_images_'s, not this
        // slot's), the one being decoded for it (its name and files, and the loader generation last looked at), the
        // part of the texture it covers, its [filtering], and the lowest row drawn in through mode (512-tall textures
        // are hashed down to it).
        bool mips{};
        std::uint8_t filter{};
        float uv_scale[2]{1.0f, 1.0f};
        std::string pending, shown; // being decoded; on the GPU
        std::vector<std::filesystem::path> pending_files;
        std::uint64_t pending_generation{~0ull};
        std::uint16_t max_seen_v{}, keyed_max_v{};
        float holes{-1.0f}; // projected shadows: the share of texels clearly transparent (alpha < 128); < 0: not counted yet
        int zone{-1};       // projected shadows: the part of a map its pack image belongs to, by its folder (-1: none)
    };

    void prepare(const gpu::Ge &ge) {
        if (have_state_ && prepared_serial_ == draw_serial_) return; // the same draw: the same state
        prepared_serial_ = draw_serial_;
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
        // Texture packs hash 512-tall textures drawn in through mode only down to the lowest row drawn.
        if (seen_v_ != nullptr && through_ && v.v > static_cast<float>(*seen_v_)) *seen_v_ = static_cast<std::uint16_t>(std::min(512.0f, std::ceil(v.v)));
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
    void copy_shown_to_vram(RenderTarget *shown);
    bool present_interpolated(RenderTarget &shown, const float uv_max[2]);
    [[nodiscard]] float phase_at(std::chrono::steady_clock::time_point when) const;
    void show_between(float phase);
    void show_final();
    void flush_pictures();
    void ensure_slot(Slot &slot, GLsizei w, GLsizei h);
    void render_between(const RenderTarget &shown, const std::vector<TapeEntry> &now,
                        const std::vector<TapeEntry> &before, const Slot &before_slot, float t);
    void draw_batch();
    std::unordered_map<std::uint64_t, std::size_t> no_depth_triangles_; // test: see draw_batch
    std::uint32_t no_depth_pictures_{};
    bool shadow_effect(const DrawSetup &setup, const GpuVertex *vertices, std::size_t count);
    void read_back();
    RenderTarget &target_for(std::uint32_t address, std::uint32_t stride, std::uint32_t format,
                             std::uint32_t needed_height);
    RenderTarget *find_target(std::uint32_t vram_offset, bool drawn_only);
    void allocate(RenderTarget &rt, std::uint32_t height);
    GLuint texture_for(const gpu::Ge &ge, float uv_xform[4]);
    void upload_rect(RenderTarget &rt, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h);
    void copy_to_ram(RenderTarget &from, std::uint32_t src, std::uint32_t sx, std::uint32_t sy, std::uint32_t dst,
                     std::uint32_t dst_stride, std::uint32_t dx, std::uint32_t dy, std::uint32_t width, std::uint32_t height);
    GLuint compile(const char *vertex, const char *fragment);
    GLint uniform(GLuint program, const char *name) { return gl_.GetUniformLocation(program, name); }
    void set_texture_params(bool linear, bool clamp_s, bool clamp_t, bool mips = false);
    // Texture packs (frontend/texture_pack.hpp, texture_images.hpp): opening the pack, its key for the current texture
    // and the image for it, putting a decoded image on the GPU, dumping the game's textures for pack makers, and
    // keeping pack images within their memory budget.
    void open_texture_pack();
    void look_up_pack(const gpu::Ge &ge, const gpu::Ge::TextureInfo &info, CachedTexture &t);
    void upload_pack_image(CachedTexture &t, const std::string &name, const textures::Image &image);
    void use_pack_image(CachedTexture &t, const std::string &name);
    void release_texture(CachedTexture &t);
    void dump_texture(const gpu::Ge &ge, const gpu::Ge::TextureInfo &info, const std::uint32_t *pixels);
    void drop_textures();
    void trim_pack_memory();
    // Debugging: MHF_GL_DUMP=<folder> writes every MHF_GL_DUMP_EVERY-th (default 60) presented frame as a PPM image,
    // starting at presented frame MHF_GL_DUMP_FROM (default 0).
    void dump_window(int w, int h);
    void draw_overlay();

    SDL_Window *window_;
    SDL_GLContext context_{};
    int scale_;
    psprecomp::GuestMemory &memory_;
    LiveSettings own_live_;
    LiveSettings *live_;
    OverlayImage *overlay_ = nullptr;
    // The blurred background of the in-game menu (made once per menu, from the picture the game was paused on).
    static constexpr GLsizei kBlurW = 320, kBlurH = 180;
    GLuint blur_program_{}, blur_texture_[2]{}, blur_fbo_[2]{};
    GLint blur_uv_off_{}, blur_uv_scale_{}, blur_tex_{}, blur_dir_{}, blur_dim_{};
    GLuint blur_source_{}; // the picture the blur was made from
    bool blur_ready_ = false;
    bool redraw_only_ = false; // show() draws the same picture again (tick with nothing new): the blur can be kept
    GLuint last_texture_{}; // the picture shown last, kept on screen while the game is paused behind an overlay
    float last_uv_[2] = {1.0f, 1.0f};
    std::chrono::steady_clock::time_point last_show_{};
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
    std::unique_ptr<textures::Pack> pack_;     // the texture pack, when one was given and could be opened
    std::unique_ptr<textures::Loader> loader_; // decodes its images
    bool pack_on_ = false;                     // drawing it (the player can turn it off in the menu)
    std::size_t pack_bytes_ = 0, pack_budget_ = std::size_t(1024) << 20; // pack images on the GPU, and their limit
    // Pack images on the GPU, one texture (with its mip levels) per image, shared by every cached texture drawn with it:
    // the game putting another texture at an address and back, or two keys naming one image, do not upload it again
    // (ibonplix's idea in pull request #39).
    struct PackImage {
        GLuint texture{};
        std::size_t bytes{};
        std::uint64_t last_use{};
    };
    std::unordered_map<std::string, PackImage> pack_images_;
    std::uint64_t pack_reused_ = 0;
    std::uint16_t *seen_v_ = nullptr;          // the lowest through-mode row drawn of the bound texture (packs)
    // Pack images put on the GPU this flip: a scene arriving with dozens of them spreads them over a few flips instead
    // of holding one up (each is an upload and its mip levels).
    static constexpr unsigned kPackUploadsPerFlip = 6;
    unsigned pack_uploads_ = 0;
    bool pack_log_ = false;                    // MHF_TEXTURE_LOG=1: each texture's key and what the pack does with it
    std::uint64_t pack_logged_at_ = 0, pack_replaced_ = 0, pack_missed_ = 0;
    std::filesystem::path dump_folder_;        // MHF_TEXTURE_DUMP=<folder>: the game's textures as a pack to edit
    std::unique_ptr<textures::Pack> dump_keys_;
    std::set<std::string> dumped_;
    // Textures replaced by new content while a recorded batch (frame interpolation) may still draw with them; they
    // are deleted a few flips later.
    std::vector<std::pair<std::uint64_t, GLuint>> retired_textures_;
    GLuint copy_texture_{}, copy_fbo_{};               // for sampling the target being drawn
    std::uint32_t copy_width_{}, copy_height_{};
    GLuint upload_texture_{};                          // CPU pixels on their way to a target or the screen
    GLuint display_texture_{};
    // The shown picture on its way back to guest VRAM (copy_shown_to_vram): a 480x272 target and a pixel buffer read one
    // flip later.
    GLuint readback_texture_{}, readback_fbo_{}, readback_pbo_{};
    struct PendingReadback {
        bool active{};
        GLsync fence{};
        std::uint32_t address{}, stride{}, format{}, width{}, height{};
    } pending_readback_;
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
    // The second design of frame interpolation (frame_matching.hpp): the draws of the flip being drawn and of the one
    // before, and how they match (MHF_INTERP_V2_LOG=1 reports it).
    std::vector<interpolation::DrawSummary> recording_, summaries_before_;
    interpolation::Matcher matcher_;
    interpolation::CutThresholds cut_thresholds_;
    void match_v2(std::uint32_t shown);
    // Interpolation v2 (the default; MHF_INTERP_V1=1 goes back to placing the recorded points again for every batch):
    // the batch's triangles before clipping, the viewports of the draws,
    // and for the flip being shown: the previous flip's draws, how they match, the camera's motion, and per draw of
    // this flip its partner in the previous one (-1 none).
    const bool v2_enabled_ = std::getenv("MHF_INTERP_V1") == nullptr;
    std::vector<RawTriangle> raw_batch_;
    bool raw_complete_ = true;
    std::uint32_t draw_triangles_ = 0; // of the draw going on
    std::uint64_t draw_serial_ = 0, prepared_serial_ = ~0ull;
    // Where each recorded vertex came from (Origin) is for the first design's placing again: with v2, only the 2D
    // batches and the 3D ones v2 cannot make again need it.
    bool needs_origins() const { return interpolate_ && (!v2_enabled_ || through_ || !raw_complete_); }
    void raw_incomplete() {
        if (!raw_complete_) return;
        raw_complete_ = false;
        if (interpolate_ && v2_enabled_) batch_origins_.resize(batch_.size()); // drawn where they were recorded
    }
    std::vector<std::array<float, 8>> recording_viewports_, viewports_now_;
    std::vector<interpolation::DrawSummary> v2_older_;
    std::vector<std::int32_t> v2_older_of_;
    interpolation::RigidMotion v2_camera_;
    bool v2_ready_ = false; // the flip being shown matched the one before, without a cut
    // What of each draw's transform does not depend on the moment, worked out once a flip (prepare_v2): a point in
    // the world goes back to the model (back), then to eye space at the moment (blended from older_eye to own, kept
    // at own, or following the camera), then through the projection. Fixed draws keep the transform they were drawn
    // with.
    struct V2Draw {
        enum Kind : std::uint8_t { kFixed, kStill, kBlend, kFollow } kind = kFixed;
        interpolation::Matrix fixed, older_eye, own, back, projection_old, projection_new;
        // An animated (skinned) draw whose partner drew as many triangles: each point is blended between where it was
        // in the model then (back_old) and now, its triangles found again by their order in the draw.
        bool deform = false;
        std::size_t old_first = 0, new_first = 0;
        interpolation::Matrix back_old;
    };
    // A share of a batch's triangles, placed by one thread into its own vertices.
    struct V2Chunk {
        std::size_t batch, from, to;
    };
    bool v2_prepared_ = false;
    std::vector<V2Draw> v2_draws_;
    std::vector<std::uint32_t> v2_needed_; // the draws the shown picture's 3D batches come from
    std::vector<V2Chunk> v2_chunks_;
    std::vector<std::size_t> v2_first_chunk_; // per batch
    std::vector<std::vector<GpuVertex>> v2_chunk_out_;
    std::vector<interpolation::Matrix> v2_transforms_; // per draw: world point to clip space at the picture's moment
    void prepare_v2(const std::vector<TapeEntry> &now, const std::vector<TapeEntry> &before);
    // The triangles of the previous flip in drawing order, and per draw of either flip where its triangles start and
    // how many there are; per batch of this flip, the order of its first triangle.
    std::vector<const RawTriangle *> v2_old_tris_;
    std::vector<std::size_t> v2_old_first_, v2_old_count_, v2_new_first_, v2_new_count_, v2_batch_ordinal_;
    std::vector<interpolation::Matrix> v2_view_t_; // per deforming draw: eye space to clip space at the moment
    float v2_t_ = 1.0f;
    void prepare_v2_draw(std::uint32_t n);
    void place_v2(std::size_t batch, const TapeEntry &entry, std::size_t from, std::size_t to, std::vector<GpuVertex> &out) const;
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
        ShadowPass shadow; // the shadows of this flip's picture, for the in-between ones
        float uv_max[2]{1, 1};
    } flip_;
    // The flip being shown, in time: in-between pictures go out as often as the drawing thread can, at most one per
    // step (the chosen rate), each one placed where things are at the moment it is drawn; there is no fixed slot a
    // late picture could miss, so the output rate is free between the game's 30 and the chosen one.
    struct {
        bool active{};
        std::chrono::steady_clock::time_point start{}, next_due{};
        std::chrono::steady_clock::duration interval{}, step{};
    } pacing_;
    std::uint64_t log_shown_{};                       // shown_ at the last log line
    std::chrono::steady_clock::time_point log_time_{}; // and when
    // In-between pictures are built in one buffer, uploaded once, and drawn batch by batch from ranges of it.
    std::vector<GpuVertex> staging_;
    struct Range {
        std::size_t first{}, count{};
    };
    std::vector<Range> ranges_;
    // What an in-between picture costs the drawing thread (MHF_INTERP_LOG=1): calculating the vertices, uploading them,
    // setting the state of each batch, and the draw calls.
    struct PictureCost {
        std::uint64_t pictures{}, batches{}, vertices{}, shows{};
        double calc_us{}, upload_us{}, state_us{}, draw_us{};
        double show_us{}, swap_us{}; // putting a picture on the window, and the buffer swap inside it (waits for the GPU)
    } cost_;
    std::chrono::steady_clock::time_point last_flip_{};
    std::uint64_t shown_{1};                // pictures put on screen
    struct {
        std::uint64_t interpolated{}, skipped_shape{}, skipped_copy{}, skipped_timing{}, skipped_first{}, skipped_ui{}, late{};
    } stats_;
    bool have_state_{};
    bool through_{};
    float tex_width_{1}, tex_height_{1};
    bool tex_mips_ = false;          // the texture texture_for() gave has mip levels (a pack image)
    std::uint8_t tex_filter_ = 0;    // and its pack [filtering]
    float tex_holes_{-1.0f};         // the share of transparent texels of the texture texture_for() gave
    float shadow_texture_holes_{-1.0f}; // the same for the draw being captured
    std::uint64_t tex_hash_{}, shadow_texture_hash_{}; // the game's texture, as the content hash the cache keys it by
    std::uint64_t shadow_pick_logged_{};
    int tex_zone_{-1}, shadow_texture_zone_{-1}; // the zone of the texture texture_for() gave; of the draw being captured
    std::unordered_map<std::uint32_t, int> shadow_zone_by_texture_; // LiveSettings::shadow_zone_textures
    std::unique_ptr<textures::Pack> shadow_zone_keys_;
    std::vector<std::string> shadow_zone_names_;
    int shadow_zone_{-1}, shadow_zone_next_{-1}, shadow_zone_votes_{}; // the zone the game is in, and the one that may follow it
    std::chrono::steady_clock::time_point shadow_last_scene_{}; // when the last 3D scene was mapped
    int shadow_floor_zone_{-1};        // the zone shadow_floor_ was taken in
    float shadow_floor_{};             // the height of the ground in it, from the world's origin: the hunter's feet on arrival
    std::vector<std::uint32_t> shadow_zone_triangles_; // how many textures of each zone are on the screen
    std::vector<int> shadow_zone_totals_;              // and how many it has
    std::vector<std::pair<int, std::uint64_t>> shadow_zone_seen_;
    std::vector<GpuVertex> batch_;
    std::vector<Origin> batch_origins_; // frame interpolation: one per vertex of batch_
    // Projected shadows (LiveSettings says what the player can change). MHF_SHADOW_REACH, _BIAS, _DROP and _SOFT, read once,
    // tune the rest. Two maps: the picture being drawn must not overwrite the one the in-between pictures of the last flip use.
    bool shadow_draw_ok_{}, shadow_tried_{}, shadow_ready_{}, shadow_first_triangle_{}, shadow_light_ok_{};
    std::uint32_t shadow_group_{}, shadow_scene_address_{};
    std::uint64_t shadow_capture_frame_{}, shadow_scene_frame_{}, shadow_done_frame_{}, shadow_generation_{};
    std::vector<ShadowGroup> shadow_groups_;
    std::vector<ShadowDraw> shadow_draws_;
    std::vector<float> shadow_vertices_; // eye x, y, z and u, v
    std::vector<ShadowRange> shadow_ranges_;
    std::unordered_set<std::uint64_t> shadow_list_air_, shadow_list_solid_, shadow_list_cave_, shadow_list_haze_; // LiveSettings::shadow_*_textures
    bool shadow_cave_textures_{};      // a texture of a cave is on the screen
    bool shadow_sliders_in_cave_{};    // the size sliders were last set for a cave
    bool shadow_in_cave_{};            // rock overhead: only objects cast
    int shadow_cave_votes_{};
    std::uint32_t shadow_frame_count_{};
    std::vector<float> shadow_depths_;          // the depth (0..1) each captured vertex was drawn with; < 0: not known
    float shadow_scene_view_[12]{};             // projection, viewport depth and viewport of the last draw of the scene itself
    float shadow_draw_vz_[2]{};                 // the viewport depth of the draw being captured
    std::uint32_t shadow_draw_vertices_{};      // and where its vertices are
    std::uint32_t shadow_light_colours_[4]{};
    bool shadow_night_{};                       // the zones' suns are for the day: at night the game's own light stays
    int shadow_night_pictures_{};
    bool shadow_night_settled_{};
    std::uint32_t shadow_skipped_{};            // draws of the picture left out for not writing the depth
    struct UndepthedDraw {                      // what those were, for the test views
        std::uint64_t texture{};
        std::uint32_t paint[6]{};
        float eye[3]{}, alpha{}, red{};
    };
    std::vector<UndepthedDraw> shadow_undepthed_;
    // Scenery that is not looked at again: picking its triangles up is the dearest part of the shadows, and between two rounds of
    // the side shade nothing needs them once its shadows are kept. By its vertices, texture and place among the objects.
    struct KnownPiece {
        float low[3]{}, high[3]{};
        std::uint32_t triangles{};
    };
    std::unordered_map<std::uint64_t, KnownPiece> shadow_known_;
    bool shadow_leave_known_{};
    std::uint32_t shadow_left_known_{};
    int shadow_known_zone_{-2};
    float shadow_fog_[2]{};                 // the game's fog end and scale, from the scene's last draw with fog
    std::uint64_t shadow_fog_frame_{~0ull}; // the picture it was taken in
    float shadow_storm_darkness_{-1.0f};
    float shadow_darkness_shown_{-1.0f}; // the darkness the shadows were last drawn with, shown top right (test) // the darkness in a sandstorm, as it eases to where it goes (-1: none)
    std::chrono::steady_clock::time_point shadow_storm_time_{};
    std::vector<std::uint32_t> shadow_sources_; // for each captured vertex, its point (shared points, for smooth normals)
    std::vector<float> shadow_normals_, shadow_facing_; // eye x, y, z and how much sun the surface faces
    std::unordered_map<std::uint64_t, std::uint32_t> shadow_points_;
    GLuint shadow_facing_program_{}, shadow_facing_vao_[2]{}, shadow_facing_vbo_[2]{};
    // Where the shadows' time goes, written to the log. On the graphics card, in order: map, hunter's map, factor, facing, applying.
    struct ShadowTime {
        GLuint query{};
        bool pending{};
        double ms{};
        int count{};
    };
    ShadowTime shadow_time_[5]{};
    int shadow_time_now_{-1};
    std::uint32_t shadow_capture_seen_{};
    double shadow_capture_ms_{}, shadow_cpu_ms_[4]{};
    int shadow_cpu_runs_{};
    GLsizei shadow_facing_count_[2]{}; // by buffer: a picture keeps its own triangles while the next ones are made
    int shadow_facing_held_{}, shadow_facing_age_{};  // the buffer in use, and for how many flips it has been reused
    bool shadow_facing_reusable_{};
    interpolation::Matrix shadow_facing_eye_{};       // the eye space it was worked out in, seen from the scenery's model
    float shadow_facing_dir_[3]{};
    GLsizei shadow_clear_count_[2]{};  // and after them, the listed pieces of the air: no shadow falls on those
    float shadow_light_[3]{}, shadow_proj_[6]{}, shadow_vz_[2]{}, shadow_vp_[4]{};
    float shadow_reach_{16000.0f}, shadow_bias_{12.0f}, shadow_drop_{8000.0f}, shadow_soft_{1.0f}, shadow_min_elevation_{35.0f};
    GLsizei shadow_size_{4096}, shadow_w_{}, shadow_h_{};
    int shadow_quality_used_{2};
    GLuint shadow_map_program_{}, shadow_factor_program_{}, shadow_apply_program_{}, shadow_show_program_{}, shadow_vao_{},
        shadow_vbo_{}, shadow_fbo_[2]{}, shadow_depth_[2]{}, shadow_color_[2]{}, shadow_factor_fbo_{}, shadow_factor_tex_{};
    int shadow_cur_{};
    ShadowPass shadow_pass_;
    ShadowKept shadow_kept_[3];        // by generation: the flip shown, the one before it, and the one being drawn
    std::vector<float> shadow_between_;
    // The scenery seen so far in this zone, in the frame of one of its pieces (the game only draws what the camera sees, and
    // keeps the camera in every matrix), and its map, drawn again only when it has to be.
    std::vector<float> shadow_seen_vertices_;              // x, y, z and u, v in that frame
    std::vector<KeptRange> shadow_seen_ranges_;
    std::unordered_set<std::uint64_t> shadow_seen_keys_;
    std::unordered_set<std::uint32_t> shadow_seen_anchor_; // the vertices of the pieces that give the frame
    ShadowKept shadow_seen_map_;
    GLuint shadow_seen_fbo_{}, shadow_seen_depth_{}, shadow_seen_color_{};
    bool shadow_seen_built_{}, shadow_seen_grew_{};
    int shadow_seen_age_{}, shadow_seen_doubts_{}, shadow_seen_builds_{};
    float shadow_seen_center_[3]{}, shadow_seen_right_[3]{}, shadow_seen_up_[3]{}, shadow_seen_dir_[3]{};
    std::vector<std::uint32_t> v2_scene_votes_; // the draws that say how the ground moved in an in-between picture
    double shadow_scene_share_{};               // how much of them agreed, added up, and over how many pictures
    int shadow_scene_pictures_{};
    // The player moves fastest and is seen closest: a small map of its own, drawn again for every in-between picture.
    static constexpr GLsizei kShadowPlayerSize = 1024;
    GLuint shadow_player_fbo_{}, shadow_player_depth_{}, shadow_player_color_{};
    std::vector<std::uint32_t> shadow_player_groups_;
    std::uint64_t frame_{1};
};

bool GlBackend::init() {
    {
        const auto number = [](const char *name, float fallback) {
            const char *e = std::getenv(name);
            return e != nullptr ? static_cast<float>(std::atof(e)) : fallback;
        };
        if (const char *on = std::getenv("MHF_SHADOW"); on != nullptr && on[0] != '0') live_->shadow = true;
        live_->shadow_strength = std::clamp(number("MHF_SHADOW_STRENGTH", live_->shadow_strength.load()), 0.0f, 1.0f);
        live_->shadow_max_object = number("MHF_SHADOW_MAX_OBJECT", live_->shadow_max_object.load());
        live_->shadow_max_piece = number("MHF_SHADOW_MAX_PIECE", live_->shadow_max_piece.load());
        if (const char *whole = std::getenv("MHF_SHADOW_WHOLE_MAP"); whole != nullptr) live_->shadow_whole_map = whole[0] != '0';
        if (const char *show = std::getenv("MHF_SHADOW_SHOW"); show != nullptr && show[0] != '0') live_->shadow_show = true;
        shadow_list_air_.insert(live_->shadow_air_textures.begin(), live_->shadow_air_textures.end());
        shadow_list_solid_.insert(live_->shadow_solid_textures.begin(), live_->shadow_solid_textures.end());
        shadow_list_cave_.insert(live_->shadow_cave_textures.begin(), live_->shadow_cave_textures.end());
        shadow_list_haze_.insert(live_->shadow_haze_textures.begin(), live_->shadow_haze_textures.end());
        shadow_zone_names_ = live_->shadow_zone_names;
        shadow_zone_totals_.assign(shadow_zone_names_.size(), 0);
        for (const auto &[hash, zone] : live_->shadow_zone_textures) {
            shadow_zone_by_texture_.emplace(hash, zone);
            ++shadow_zone_totals_[static_cast<std::size_t>(zone)];
        }
    }
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
    blur_program_ = compile(kBlurVertex, kBlurFragment);
    if (draw_program_ == 0 || present_program_ == 0 || overlay_program_ == 0 || blur_program_ == 0) return false;
    blur_uv_off_ = uniform(blur_program_, "u_uv_off");
    blur_uv_scale_ = uniform(blur_program_, "u_uv_scale");
    blur_tex_ = uniform(blur_program_, "u_tex");
    blur_dir_ = uniform(blur_program_, "u_dir");
    blur_dim_ = uniform(blur_program_, "u_dim");
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

// An RGBA8 pixel read from a render target, written in the target's own format (2 or 4 bytes).
void pack_pixel(const std::uint8_t *p, std::uint32_t format, std::uint8_t *out) {
    if (format == 3) {
        std::memcpy(out, p, 4);
        return;
    }
    std::uint16_t v;
    if (format == 0) v = static_cast<std::uint16_t>((p[0] >> 3) | ((p[1] >> 2) << 5) | ((p[2] >> 3) << 11));
    else if (format == 1) v = static_cast<std::uint16_t>((p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10) | ((p[3] >> 7) << 15));
    else v = static_cast<std::uint16_t>((p[0] >> 4) | ((p[1] >> 4) << 4) | ((p[2] >> 4) << 8) | ((p[3] >> 4) << 12));
    std::memcpy(out, &v, 2);
}

// ---------------------------------------------------------------- render targets

void GlBackend::allocate(RenderTarget &rt, std::uint32_t height) {
    const GLsizei w = static_cast<GLsizei>(rt.width) * scale_, h = static_cast<GLsizei>(height) * scale_;
    GLuint color = 0, depth = 0, fbo = 0;
    gl_.GenTextures(1, &color);
    gl_.BindTexture(GL_TEXTURE_2D, color);
    gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    set_texture_params(true, true, true);
    // The depth is a texture (not a renderbuffer) so that the projected shadows can read it.
    gl_.GenTextures(1, &depth);
    gl_.BindTexture(GL_TEXTURE_2D, depth);
    gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    set_texture_params(false, true, true);
    gl_.GenFramebuffers(1, &fbo);
    gl_.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
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
        gl_.DeleteTextures(1, &rt.depth);
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
        gl_.DeleteTextures(1, &old.depth);
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
            gl_.DeleteTextures(1, &victim->second.depth);
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

void GlBackend::set_texture_params(bool linear, bool clamp_s, bool clamp_t, bool mips) {
    // Texture pack images carry mip levels: at a distance they are sampled from those, not shimmering.
    const GLint min_filter = mips ? (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR) : (linear ? GL_LINEAR : GL_NEAREST);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp_s ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp_t ? GL_CLAMP_TO_EDGE : GL_REPEAT);
}

textures::TextureFacts texture_facts(const gpu::Ge &ge, const gpu::Ge::TextureInfo &info, std::uint16_t max_seen_v);

GLuint GlBackend::texture_for(const gpu::Ge &ge, float uv_xform[4]) {
    tex_mips_ = false;
    tex_filter_ = 0;
    seen_v_ = nullptr;
    // The player turned the pack on or off: every texture is uploaded again, as the game's or as the pack's.
    if (const bool want = pack_ != nullptr && live_->texture_pack.load(std::memory_order_relaxed); want != pack_on_) {
        pack_on_ = want;
        drop_textures();
        if (!pack_on_) loader_->clear();
    }
    const auto info = ge.texture_info();
    tex_width_ = static_cast<float>(info.width);
    tex_height_ = static_cast<float>(info.height);
    uv_xform[0] = 1.0f;
    uv_xform[1] = 1.0f;
    uv_xform[2] = 0.0f;
    uv_xform[3] = 0.0f;
    const bool indexed = info.format >= 4 && info.format <= 7;
    if (indexed && (info.address & 0x0F000000u) == 0x04000000u && find_target(info.address & 0x001FFFFFu, true) != nullptr) {
        // A palette texture over a framebuffer the GE drew is read from VRAM, which only holds the screen copy (late, and
        // at the PSP's resolution). Named once per address, so an effect that looks wrong can be traced to it.
        static std::set<std::uint32_t> reported;
        if (reported.insert(info.address).second) {
            std::fprintf(stderr, "[gl] palette texture read from a drawn framebuffer at %08X (format %u, %ux%u)\n", info.address,
                         info.format, info.width, info.height);
        }
    }
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
        // A 512-tall texture drawn lower than before has another pack key.
        const bool rekey = pack_on_ && info.height == 512 && t.max_seen_v != t.keyed_max_v;
        if (t.texture == 0 || hash != t.hash || rekey) {
            t.hash = hash;
            t.holes = -1.0f; // the content changed: counted again below
            t.keyed_max_v = t.max_seen_v;
            std::shared_ptr<const textures::Image> image;
            bool kept = false;
            if (pack_on_) {
                look_up_pack(ge, info, t);
                if (!t.pending.empty() && pack_images_.count(t.pending) != 0) {
                    use_pack_image(t, t.pending); // on the GPU already: no decoding, no upload
                    t.pending.clear();
                    kept = true;
                } else if (!t.pending.empty() && (pack_uploads_ >= kPackUploadsPerFlip ||
                                                  loader_->get(t.pending, t.pending_files, image) != textures::Loader::State::Ready)) {
                    image.reset();
                }
            }
            if (kept) {
            } else if (image) {
                upload_pack_image(t, t.pending, *image);
                loader_->forget(t.pending);
                t.pending.clear();
            } else {
                // The game's own texture: until the pack's image is decoded, or for good.
                pixels_.resize(static_cast<std::size_t>(info.width) * info.height);
                ge.decode_texture(pixels_.data());
                if (!dump_folder_.empty()) dump_texture(ge, info, pixels_.data());
                if (!t.shown.empty() || interpolate_) release_texture(t); // a pack image's, or one a batch may replay
                if (t.texture == 0) gl_.GenTextures(1, &t.texture);
                t.mips = false;
                gl_.BindTexture(GL_TEXTURE_2D, t.texture);
                gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(info.width), static_cast<GLsizei>(info.height),
                               0, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
            }
        }
    }
    // The pack's image arrived (the loader moves on only when one finished): it takes the game's texture's place.
    if (!t.pending.empty() && pack_on_ && loader_->generation() != t.pending_generation && pack_uploads_ < kPackUploadsPerFlip) {
        t.pending_generation = loader_->generation();
        std::shared_ptr<const textures::Image> image;
        const auto state = pack_images_.count(t.pending) != 0 ? textures::Loader::State::Ready : loader_->get(t.pending, t.pending_files, image);
        if (state == textures::Loader::State::Ready) {
            if (image) upload_pack_image(t, t.pending, *image);
            else use_pack_image(t, t.pending); // another texture brought it to the GPU meanwhile
            loader_->forget(t.pending);
            t.pending.clear();
        } else if (state == textures::Loader::State::Failed) {
            t.pending.clear(); // the game's texture stays
        }
    }
    uv_xform[0] = t.uv_scale[0];
    uv_xform[1] = t.uv_scale[1];
    tex_mips_ = t.mips;
    tex_filter_ = t.filter;
    if (pack_on_ && info.height == 512) seen_v_ = &t.max_seen_v;
    if (t.holes < 0.0f && live_->shadow.load(std::memory_order_relaxed)) {
        // Which textures really have holes (leaves, fronds): the game leaves the alpha test on for plain rock and ground too.
        if (info.format == 0) {
            t.holes = 0.0f; // 5650: no alpha
        } else {
            pixels_.resize(static_cast<std::size_t>(info.width) * info.height);
            ge.decode_texture(pixels_.data());
            std::size_t clear = 0;
            for (const std::uint32_t px : pixels_) clear += (px >> 24u) < 128u ? 1u : 0u;
            t.holes = pixels_.empty() ? 0.0f : static_cast<float>(clear) / static_cast<float>(pixels_.size());
        }
        // Which zone of which map it belongs to, by the game's own texture: no pack is needed for this.
        t.zone = -1;
        if (!shadow_zone_by_texture_.empty()) {
            if (!shadow_zone_keys_) shadow_zone_keys_ = textures::Pack::keys_only();
            textures::Key key;
            std::uint32_t covered_w = info.width, covered_h = info.height;
            const auto memory = [&ge](std::uint32_t address, std::uint32_t size) { return ge.guest_bytes(address, size); };
            if (shadow_zone_keys_->key_of(texture_facts(ge, info, t.max_seen_v), memory, key, covered_w, covered_h)) {
                if (const auto zone = shadow_zone_by_texture_.find(key.data); zone != shadow_zone_by_texture_.end()) t.zone = zone->second;
            }
        }
    }
    tex_holes_ = t.holes;
    tex_hash_ = t.hash;
    tex_zone_ = t.zone;
    return t.texture;
}

// ---------------------------------------------------------------- texture packs

void GlBackend::open_texture_pack() {
    pack_log_ = std::getenv("MHF_TEXTURE_LOG") != nullptr || std::getenv("MHF_TEXTURE_DEBUG") != nullptr; // the latter: PR #39's name
    if (const char *mb = std::getenv("MHF_TEXTURE_PACK_MEMORY")) pack_budget_ = std::size_t(std::max(64L, std::atol(mb))) << 20;
    std::string folder = live_->texture_pack_folder;
    if (folder.empty()) {
        if (const char *env = std::getenv("MHF_TEXTURE_PACK")) folder = env;
    }
    if (const char *dump = std::getenv("MHF_TEXTURE_DUMP")) {
        dump_folder_ = textures::from_utf8(dump);
        std::error_code ec;
        std::filesystem::create_directories(dump_folder_, ec);
    }
    if (!folder.empty()) {
        std::string error;
        const std::filesystem::path chosen = textures::from_utf8(folder);
        std::filesystem::path located = textures::Pack::locate(chosen, live_->texture_pack_game);
        pack_ = textures::Pack::open(located.empty() ? chosen : located, live_->texture_pack_game, error);
        if (!pack_) {
            std::fprintf(stderr, "[textures] no texture pack: %s\n", error.c_str());
        } else {
            std::fprintf(stderr, "[textures] texture pack: %zu textures from %s\n", pack_->size(), textures::utf8(pack_->ini()).c_str());
            for (const std::string &w : pack_->warnings()) std::fprintf(stderr, "[textures] %s\n", w.c_str());
            const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
            loader_ = std::make_unique<textures::Loader>(std::clamp(cores / 3u, 1u, 3u));
        }
    }
    // Dumps take their keys from the pack's options when there is a pack (so edited images fit in it), else from a
    // new pack's: the quick hash, as PPSSPP writes them.
    if (!dump_folder_.empty() && !pack_) {
        std::error_code ec;
        const std::filesystem::path ini = dump_folder_ / "textures.ini";
        if (!std::filesystem::exists(ini, ec)) {
            std::ofstream(ini, std::ios::binary) << "# Textures dumped by MHForever (MHF_TEXTURE_DUMP). Edit the images: their names are\n"
                                                    "# their keys, so the folder is a texture pack as it is.\n"
                                                    "[options]\nversion = 1\nhash = quick\n\n[hashes]\n";
        }
        dump_keys_ = textures::Pack::keys_only();
    }
}

// The facts of the current texture a pack's key is made of.
textures::TextureFacts texture_facts(const gpu::Ge &ge, const gpu::Ge::TextureInfo &info, std::uint16_t max_seen_v) {
    textures::TextureFacts f;
    f.address = info.address;
    f.buffer_width = info.stride;
    f.format = info.format;
    f.width = info.width;
    f.height = info.height;
    f.dimension = static_cast<std::uint16_t>(ge.reg(kTexSize0) & 0x0F0Fu);
    f.max_seen_v = max_seen_v;
    f.clut_format_word = (kClutFormat << 24u) | (ge.reg(kClutFormat) & 0xFFFFFFu);
    f.clut = ge.clut_bytes();
    f.clut_last_bytes = ge.clut_last_bytes();
    f.clut_max_bytes = ge.clut_max_bytes();
    return f;
}

void GlBackend::look_up_pack(const gpu::Ge &ge, const gpu::Ge::TextureInfo &info, CachedTexture &t) {
    t.pending.clear();
    t.pending_files.clear();
    t.pending_generation = ~0ull;
    t.filter = 0;
    t.uv_scale[0] = t.uv_scale[1] = 1.0f;
    textures::Key key;
    std::uint32_t covered_w = info.width, covered_h = info.height;
    const auto memory = [&ge](std::uint32_t address, std::uint32_t size) { return ge.guest_bytes(address, size); };
    if (!pack_->key_of(texture_facts(ge, info, t.max_seen_v), memory, key, covered_w, covered_h)) return;
    const textures::Match m = pack_->find(key);
    if (pack_log_) {
        std::fprintf(stderr, "[textures] %s %ux%u format %u: %s\n", textures::format_key(key).c_str(), info.width, info.height,
                     info.format, m.entry == nullptr ? "not in the pack" : m.entry->keeps_original() ? "kept" : m.entry->image().c_str());
    }
    if (m.entry == nullptr || m.entry->keeps_original()) {
        ++pack_missed_;
        return;
    }
    t.filter = m.filter == textures::Filter::Nearest ? 1 : m.filter == textures::Filter::Linear ? 2 : 0;
    // A [hashranges] image covers only part of the texture: coordinates reach past it in the same proportion.
    t.uv_scale[0] = static_cast<float>(info.width) / static_cast<float>(std::max(1u, covered_w));
    t.uv_scale[1] = static_cast<float>(info.height) / static_cast<float>(std::max(1u, covered_h));
    t.pending = m.entry->image();
    for (const auto &[level, name] : m.entry->levels) t.pending_files.push_back(pack_->path_of(name));
}

// The cached texture lets go of its texture: its own is deleted (or retired, when a recorded batch may replay with it);
// a pack image's stays with the image.
void GlBackend::release_texture(CachedTexture &t) {
    if (t.texture != 0 && t.shown.empty()) {
        if (interpolate_) retired_textures_.emplace_back(frame_, t.texture);
        else gl_.DeleteTextures(1, &t.texture);
    }
    t.texture = 0;
    t.shown.clear();
    t.mips = false;
}

void GlBackend::use_pack_image(CachedTexture &t, const std::string &name) {
    PackImage &image = pack_images_.at(name);
    if (t.shown != name) release_texture(t);
    t.texture = image.texture;
    t.shown = name;
    t.mips = true;
    image.last_use = frame_;
    ++pack_reused_;
}

// Puts a decoded image on the GPU, once, with its mip levels, and draws the cached texture with it.
void GlBackend::upload_pack_image(CachedTexture &t, const std::string &name, const textures::Image &image) {
    if (pack_images_.count(name) == 0) {
        GLuint texture = 0;
        gl_.GenTextures(1, &texture);
        gl_.BindTexture(GL_TEXTURE_2D, texture);
        gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        for (std::size_t level = 0; level < image.levels.size(); ++level) {
            const auto w = static_cast<GLsizei>(std::max(1u, image.width >> level)), h = static_cast<GLsizei>(std::max(1u, image.height >> level));
            gl_.TexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.levels[level].data());
        }
        // An image without its own mip levels gets them made here; a full chain either way.
        if (image.levels.size() == 1) gl_.GenerateMipmap(GL_TEXTURE_2D);
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, image.levels.size() == 1 ? 1000 : static_cast<GLint>(image.levels.size() - 1));
        const std::size_t bytes = std::size_t(image.width) * image.height * 4u * 4u / 3u;
        pack_images_[name] = PackImage{texture, bytes, frame_};
        pack_bytes_ += bytes;
        ++pack_replaced_;
        ++pack_uploads_;
    }
    use_pack_image(t, name);
    --pack_reused_; // counted as an upload, not a reuse
}

// The game's texture as a PNG named by its key, once: with a textures.ini next to them the folder is a pack (named
// images are found without being listed), so editing an image replaces the texture.
void GlBackend::dump_texture(const gpu::Ge &ge, const gpu::Ge::TextureInfo &info, const std::uint32_t *pixels) {
    const textures::Pack *keys = pack_ ? pack_.get() : dump_keys_.get();
    if (keys == nullptr) return;
    textures::Key key;
    std::uint32_t covered_w = 0, covered_h = 0;
    const auto memory = [&ge](std::uint32_t address, std::uint32_t size) { return ge.guest_bytes(address, size); };
    if (!keys->key_of(texture_facts(ge, info, 0), memory, key, covered_w, covered_h)) return;
    const std::string name = textures::format_key(key);
    if (!dumped_.insert(name).second) return;
    std::error_code ec;
    const std::filesystem::path file = dump_folder_ / (name + ".png");
    if (std::filesystem::exists(file, ec)) return; // an image already edited is never written over
    textures::write_png(file, info.width, info.height, pixels);
}

// Every cached texture uploaded again at its next use (the pack was turned on or off).
void GlBackend::drop_textures() {
    for (auto &[key, t] : textures_) release_texture(t);
    textures_.clear();
    for (auto &[name, image] : pack_images_) {
        if (interpolate_) retired_textures_.emplace_back(frame_, image.texture);
        else gl_.DeleteTextures(1, &image.texture);
    }
    pack_images_.clear();
    pack_bytes_ = 0;
    seen_v_ = nullptr;
}

// Pack images go when nothing drew with them for a while, or, past the budget, those drawn least recently (not in this
// flip or the one before); the cached textures drawn with them are made again, as the game's first, when next drawn.
void GlBackend::trim_pack_memory() {
    const bool over = pack_bytes_ > pack_budget_;
    if (!over && (frame_ & 255u) != 0) return;
    for (const auto &[key, t] : textures_) { // an image is in use while a cached texture draws with it
        if (!t.shown.empty()) {
            if (const auto it = pack_images_.find(t.shown); it != pack_images_.end()) it->second.last_use = std::max(it->second.last_use, t.last_use);
        }
    }
    std::vector<std::pair<std::uint64_t, std::string>> by_age;
    for (const auto &[name, image] : pack_images_) {
        if (image.last_use + 600 < frame_ || (over && image.last_use + 1 < frame_)) by_age.emplace_back(image.last_use, name);
    }
    std::sort(by_age.begin(), by_age.end());
    const std::size_t goal = pack_budget_ / 10u * 9u;
    std::set<std::string> gone;
    for (const auto &[use, name] : by_age) {
        if (use + 600 >= frame_ && pack_bytes_ <= goal) break;
        const auto it = pack_images_.find(name);
        pack_bytes_ -= std::min(pack_bytes_, it->second.bytes);
        if (interpolate_) retired_textures_.emplace_back(frame_, it->second.texture);
        else gl_.DeleteTextures(1, &it->second.texture);
        pack_images_.erase(it);
        gone.insert(name);
    }
    if (gone.empty()) return;
    for (auto i = textures_.begin(); i != textures_.end();) {
        if (gone.count(i->second.shown) != 0) i = textures_.erase(i);
        else ++i;
    }
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
    seen_v_ = nullptr; // untextured draws say nothing of the last texture's rows
    tex_mips_ = false;
    tex_filter_ = 0;
    tex_holes_ = -1.0f;
    tex_hash_ = 0;
    tex_zone_ = -1;
    if (textured) {
        gl_.ActiveTexture(GL_TEXTURE0);
        setup.texture = texture_for(ge, setup.uv_xform);
        setup.self_sampled = setup.texture == copy_texture_;
        setup.texture_mips = tex_mips_;
        setup.texture_filter = tex_filter_;
    }
    shadow_texture_holes_ = tex_holes_;
    shadow_texture_hash_ = tex_hash_;
    shadow_texture_zone_ = tex_zone_;
    // The tower's beams of light, in the maps looked at since: the older ones stay as they were.
    bool beam = false;
    if (tex_hash_ == 0x2bd4dc20a8ac9a3bull && shadow_zone_ >= 0) {
        const std::string &map = shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)];
        beam = map.rfind("areas/hunting/tower_1_2/", 0) == 0 || map.rfind("areas/hunting/great_forest/", 0) == 0 || map.rfind("areas/hunting/old_jungle/", 0) == 0;
    }
    setup.haze = textured && (shadow_list_haze_.count(tex_hash_) != 0 || beam);
    setup.tex_hash = textured ? tex_hash_ : 0;
    setup.listed = textured && (shadow_list_air_.count(tex_hash_) != 0 || shadow_list_solid_.count(tex_hash_) != 0 ||
                                shadow_list_cave_.count(tex_hash_) != 0 || shadow_list_haze_.count(tex_hash_) != 0);
    setup.tex_width = tex_width_;
    setup.tex_height = tex_height_;
    setup_ = setup;
    // Projected shadows: the opaque 3D drawn into the screen's target is the scene; when 2D drawing starts over it (the HUD),
    // the shadows are worked out and multiplied into what was drawn so far. Once per picture.
    if (live_->shadow.load(std::memory_order_relaxed) && !clear && rt.width >= 480u && rt.width <= 512u &&
        rt.height >= 272u && rt.height <= 274u) {
        if (!through_) {
            if ((ge.reg(kZTestEnable) & 1u) != 0 && (ge.reg(kZWriteDisable) & 1u) == 0) {
                const auto &p = ge.proj_matrix().m;
                shadow_proj_[0] = p[0], shadow_proj_[1] = p[5], shadow_proj_[2] = p[10], shadow_proj_[3] = p[11],
                shadow_proj_[4] = p[14], shadow_proj_[5] = p[15];
                shadow_vz_[0] = gpu::float24(ge.reg(kViewportZScale));
                shadow_vz_[1] = gpu::float24(ge.reg(kViewportZCenter));
                if ((ge.reg(kFogEnable) & 1u) != 0) {
                    shadow_fog_[0] = gpu::float24(ge.reg(0xCDu));
                    shadow_fog_[1] = gpu::float24(ge.reg(0xCEu));
                    shadow_fog_frame_ = frame_;
                }
                if ((ge.reg(kFogEnable) & 1u) != 0) { // test: the game's fog range, when it changes (a sandstorm?)
                    static float told_end = 0.0f, told_scale = 0.0f;
                    static int told_count = 0;
                    const float end = gpu::float24(ge.reg(0xCDu)), scale = gpu::float24(ge.reg(0xCEu));
                    if (told_count < 400 && (std::fabs(end - told_end) > 0.02f * std::fabs(told_end) + 1.0f ||
                                             std::fabs(scale - told_scale) > 0.02f * std::fabs(told_scale) + 1e-9f)) {
                        told_end = end, told_scale = scale, ++told_count;
                        std::fprintf(stderr, "[shadow fog] zone %s: end %.1f scale %.8f (gone at %.0f ahead)\n",
                                     shadow_zone_ >= 0 ? shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)].c_str() : "none", end, scale,
                                     scale != 0.0f ? end - 0.0f : 0.0f);
                    }
                }
                shadow_vp_[0] = gpu::float24(ge.reg(kViewportXScale));
                shadow_vp_[1] = gpu::float24(ge.reg(kViewportYScale));
                shadow_vp_[2] = gpu::float24(ge.reg(kViewportXCenter)) - static_cast<float>(ge.reg(kOffsetX) & 0xFFFFu) / 16.0f;
                shadow_vp_[3] = gpu::float24(ge.reg(kViewportYCenter)) - static_cast<float>(ge.reg(kOffsetY) & 0xFFFFu) / 16.0f;
                shadow_scene_address_ = rt.address;
                shadow_scene_frame_ = frame_;
            }
        } else if (shadow_scene_frame_ == frame_ && shadow_done_frame_ != frame_ && shadow_scene_address_ == rt.address) {
            shadow_done_frame_ = frame_;
            run_shadow(rt);
        }
    }
    emit_state(setup, rt);
}

void GlBackend::shadow_begin_draw(const gpu::DrawInfo &draw) {
    shadow_draw_ok_ = false;
    if (!live_->shadow.load(std::memory_order_relaxed) || draw.through || draw.clear) return;
    if (shadow_capture_frame_ != frame_) {
        shadow_capture_frame_ = frame_;
        shadow_groups_.clear();
        shadow_draws_.clear();
        shadow_vertices_.clear();
        shadow_sources_.clear();
        shadow_depths_.clear();
        shadow_skipped_ = 0;
        shadow_undepthed_.clear();
        // Only while the side shade will not be worked out again, and with the scenery's shadows kept.
        static const bool never = std::getenv("MHF_SHADOW_LOOK_AT_ALL") != nullptr;
        shadow_left_known_ = 0u;
        shadow_leave_known_ = !never && !shadow_known_.empty() && shadow_seen_built_ && shadow_facing_reusable_ && shadow_facing_age_ < 3 &&
                              !live_->shadow_pick.load(std::memory_order_relaxed) && live_->shadow_keep_scenery.load(std::memory_order_relaxed);
        shadow_light_ok_ = false;
    }
    const bool skinned = ((draw.vertex_type >> 9u) & 3u) != 0;
    const auto same = [&](const ShadowGroup &g) { return g.skinned == skinned && g.world == draw.world && g.view == draw.view; };
    if (shadow_group_ >= shadow_groups_.size() || !same(shadow_groups_[shadow_group_])) {
        std::size_t i = 0;
        while (i < shadow_groups_.size() && !same(shadow_groups_[i])) ++i;
        if (i == shadow_groups_.size()) {
            if (i >= 1024u) return;
            ShadowGroup &g = shadow_groups_.emplace_back();
            g.world = draw.world;
            g.view = draw.view;
            g.skinned = skinned;
            if (i != 0u) {
                const auto &first = shadow_groups_.front().world;
                const float dx = g.world[9] - first[9], dy = g.world[10] - first[10], dz = g.world[11] - first[11];
                g.place = static_cast<std::uint64_t>(std::sqrt(dx * dx + dy * dy + dz * dz) / 16.0f + 0.5f);
            }
            for (int c = 0; c < 3; ++c) {
                const float *m = draw.world.data() + 3 * c;
                const float length = std::max(std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), 1e-6f);
                for (int k = 0; k < 3; ++k) g.axis[c][k] = m[k] / length;
            }
        }
        shadow_group_ = static_cast<std::uint32_t>(i);
    }
    shadow_draw_vertices_ = draw.vertex_address;
    shadow_draw_vz_[0] = draw.viewport[2];
    shadow_draw_vz_[1] = draw.viewport[5];
    shadow_draw_ok_ = true;
    shadow_first_triangle_ = true;
}

void GlBackend::shadow_capture(const gpu::Ge &ge, const gpu::Vertex &a, const gpu::Vertex &b, const gpu::Vertex &c) {
    const bool picking = live_->shadow_pick.load(std::memory_order_relaxed) && shadow_undepthed_.size() < 4000u;
    const auto note_left_out = [&](bool placed) {
        prepare(ge);
        UndepthedDraw u;
        u.texture = shadow_texture_hash_;
        u.paint[0] = ge.reg(kBlendMode);
        u.paint[1] = ge.reg(kBlendFixedA);
        u.paint[2] = ge.reg(kBlendFixedB);
        u.paint[3] = (ge.reg(kZTestEnable) & 1u) | (ge.reg(kZWriteDisable) & 1u) << 1u | (ge.reg(kFogEnable) & 1u) << 2u | (ge.reg(kAlphaTestEnable) & 1u) << 3u | (placed ? 16u : 0u) | (a.r < 96.0f ? 32u : 0u);
        u.paint[4] = ge.reg(kTexFunc);
        u.paint[5] = ge.reg(kAlphaTest);
        if (placed) {
            const auto &m = shadow_groups_[shadow_group_].view;
            for (int k = 0; k < 3; ++k) u.eye[k] = m[k] * a.world[0] + m[3 + k] * a.world[1] + m[6 + k] * a.world[2] + m[9 + k];
        }
        u.alpha = a.a;
        u.red = a.r;
        shadow_undepthed_.push_back(u);
    };
    if (!a.has_world || !b.has_world || !c.has_world) {
        if (picking && shadow_first_triangle_) {
            note_left_out(false);
            shadow_first_triangle_ = false;
            shadow_draw_ok_ = false;
        }
        return;
    }
    if (shadow_first_triangle_) {
        shadow_first_triangle_ = false;
        // What is not in the depth (effects, glows) casts nothing, and neither does anything drawn somewhere else.
        if ((ge.reg(kZTestEnable) & 1u) == 0 || (ge.reg(kZWriteDisable) & 1u) != 0 || shadow_vertices_.size() > 7500000u) {
            shadow_draw_ok_ = false;
            if (picking) note_left_out(true);
            ++shadow_skipped_;
            return;
        }
        prepare(ge);
        std::uint64_t known_key = 1469598103934665603ull;
        // Many pieces share their buffer of vertices: the first one read tells them apart. None if the camera cut that corner off.
        for (const std::uint64_t v : {static_cast<std::uint64_t>(shadow_draw_vertices_), static_cast<std::uint64_t>(a.source), shadow_texture_hash_, shadow_groups_[shadow_group_].place}) known_key = (known_key ^ v) * 1099511628211ull;
        if (a.source == 0u) known_key = 0u;
        // One in eight stays: they say where the scenery is now.
        if (shadow_leave_known_ && (known_key >> 40u & 7u) != 0u) {
            const auto found = shadow_known_.find(known_key);
            if (found != shadow_known_.end()) {
                ShadowGroup &g = shadow_groups_[shadow_group_];
                for (int k = 0; k < 3; ++k) {
                    g.low[k] = std::min(g.low[k], found->second.low[k]);
                    g.high[k] = std::max(g.high[k], found->second.high[k]);
                }
                g.triangles += found->second.triangles;
                ++shadow_left_known_;
                shadow_draw_ok_ = false;
                return;
            }
        }
        ShadowDraw d;
        d.known_key = known_key;
        d.group = shadow_group_;
        d.first = static_cast<std::uint32_t>(shadow_vertices_.size() / 5u);
        d.target = setup_.target;
        d.texture = setup_.texture;
        std::copy(setup_.uv_xform, setup_.uv_xform + 4, d.xform);
        // Cut-outs: the map keeps the shape the game's alpha test gives.
        const std::uint32_t tex_func = ge.reg(kTexFunc), wrap = ge.reg(kTexWrap);
        const bool texture_alpha = d.texture != 0 && (tex_func & 0x100u) != 0 && (tex_func & 7u) != 1u;
        if (texture_alpha && (ge.reg(kAlphaTestEnable) & 1u) != 0) {
            const std::uint32_t at = ge.reg(kAlphaTest);
            d.func = static_cast<std::int32_t>(at & 7u);
            d.ref = static_cast<std::int32_t>((at >> 8u) & 0xFFu);
            d.mask = static_cast<std::int32_t>((at >> 16u) & 0xFFu);
        }
        d.clamp_s = (wrap & 1u) != 0 ? 1 : 0;
        d.clamp_t = (wrap & 0x100u) != 0 ? 1 : 0;
        static const float min_holes = [] {
            const char *e = std::getenv("MHF_SHADOW_HOLES");
            return e != nullptr ? static_cast<float>(std::atof(e)) : 0.02f;
        }();
        // Plants are tested against a high reference and blended too. Layers of the ground fade with blending alone, and the
        // game leaves a low alpha test on for plain surfaces, so neither of those counts, whatever their texture looks like.
        d.blended = (ge.reg(kAlphaBlendEnable) & 1u) != 0;
        d.fog_color = ge.reg(kFogColor);
        d.paint[0] = ge.reg(kBlendMode);
        d.paint[1] = ge.reg(kBlendFixedA);
        d.paint[2] = ge.reg(kBlendFixedB);
        d.paint[3] = ge.reg(kMaskRgb);
        d.paint[4] = (ge.reg(kAlphaTestEnable) & 1u) != 0 ? ge.reg(kAlphaTest) : 0xFFFFFFFFu;
        d.paint[5] = ge.reg(kTexFunc);
        d.paint[6] = (ge.reg(kTextureMapEnable) & 1u) | (ge.reg(kFogEnable) & 1u) << 1u | (ge.reg(kStencilTestEnable) & 1u) << 2u;
        // Much of the rock is blended too, with a few clear texels along its edges: only a texture that is mostly clear is air.
        d.translucent = d.blended && shadow_texture_holes_ >= 0.25f;
        d.has_holes = shadow_texture_holes_ >= min_holes;
        d.texture_hash = shadow_texture_hash_;
        d.zone = shadow_texture_zone_;
        d.vertices = shadow_draw_vertices_;
        if (shadow_list_air_.count(d.texture_hash) != 0) d.listed = 1;
        else if (shadow_list_solid_.count(d.texture_hash) != 0) d.listed = 2;
        else if (shadow_list_cave_.count(d.texture_hash) != 0) d.listed = 3;
        d.holes = shadow_texture_holes_;
        d.cutout = (d.func == 6 || d.func == 7) && d.ref >= 64 && (ge.reg(kAlphaBlendEnable) & 1u) != 0 &&
                   shadow_texture_holes_ >= min_holes;
        shadow_draws_.push_back(d);
        // The game's light while it draws lit things: a directional light's position registers hold the direction towards
        // it, in the space the world matrix leads to.
        static const std::uint32_t light = [] {
            const char *e = std::getenv("MHF_SHADOW_LIGHT");
            return static_cast<std::uint32_t>(e != nullptr ? std::clamp(std::atoi(e), 0, 3) : 0);
        }();
        if ((ge.reg(0x17) & 1u) != 0 && (ge.reg(0x18u + light) & 1u) != 0) {
            const std::uint32_t r = 0x63u + 3u * light;
            const float x = gpu::float24(ge.reg(r)), y = gpu::float24(ge.reg(r + 1u)), z = gpu::float24(ge.reg(r + 2u));
            const auto &m = shadow_groups_[shadow_group_].view;
            shadow_light_[0] = m[0] * x + m[3] * y + m[6] * z;
            shadow_light_[1] = m[1] * x + m[4] * y + m[7] * z;
            shadow_light_[2] = m[2] * x + m[5] * y + m[8] * z;
            shadow_light_ok_ = true;
            // Its colours, and the fog's: they tell day from night.
            shadow_light_colours_[0] = ge.reg(0x5Cu);                 // the scene's ambient
            shadow_light_colours_[1] = ge.reg(0x8Fu + 3u * light);    // the light's ambient
            shadow_light_colours_[2] = ge.reg(0x90u + 3u * light);    // and diffuse
            shadow_light_colours_[3] = ge.reg(kFogColor);
        }
    }
    ShadowGroup &g = shadow_groups_[shadow_group_];
    ShadowDraw &draw = shadow_draws_.back();
    const auto &m = g.view;
    const auto &w = g.world;
    for (const gpu::Vertex *p : {&a, &b, &c}) {
        const float *q = p->world;
        const float eye[3] = {m[0] * q[0] + m[3] * q[1] + m[6] * q[2] + m[9], m[1] * q[0] + m[4] * q[1] + m[7] * q[2] + m[10],
                              m[2] * q[0] + m[5] * q[1] + m[8] * q[2] + m[11]};
        const float d[3] = {q[0] - w[9], q[1] - w[10], q[2] - w[11]};
        for (int k = 0; k < 3; ++k) {
            g.eye_low[k] = std::min(g.eye_low[k], eye[k]);
            g.eye_high[k] = std::max(g.eye_high[k], eye[k]);
            const float along = g.axis[k][0] * d[0] + g.axis[k][1] * d[1] + g.axis[k][2] * d[2];
            g.low[k] = std::min(g.low[k], along);
            g.high[k] = std::max(g.high[k], along);
            draw.low[k] = std::min(draw.low[k], along);
            draw.high[k] = std::max(draw.high[k], along);
        }
        for (const float value : {eye[0], eye[1], eye[2], p->u, p->v}) shadow_vertices_.push_back(value);
        draw.fog_low = std::min(draw.fog_low, p->fog);
        draw.fog_high = std::max(draw.fog_high, p->fog);
        draw.alpha_low = std::min(draw.alpha_low, p->a);
        draw.alpha_high = std::max(draw.alpha_high, p->a);
        shadow_depths_.push_back(p->w > 0.0f ? (p->z / p->w * shadow_draw_vz_[0] + shadow_draw_vz_[1]) / 65535.0f : -1.0f);
    }
    g.triangles += 1u;
    draw.count += 3u;
}

bool GlBackend::shadow_setup() {
    if (shadow_tried_) return shadow_ready_;
    shadow_tried_ = true;
    const auto number = [](const char *name, float fallback) {
        const char *e = std::getenv(name);
        return e != nullptr ? static_cast<float>(std::atof(e)) : fallback;
    };
    shadow_reach_ = std::max(100.0f, number("MHF_SHADOW_REACH", shadow_reach_));
    shadow_bias_ = number("MHF_SHADOW_BIAS", shadow_bias_);
    shadow_drop_ = std::max(1.0f, number("MHF_SHADOW_DROP", shadow_drop_));
    shadow_soft_ = std::max(0.0f, number("MHF_SHADOW_SOFT", shadow_soft_));
    shadow_min_elevation_ = std::clamp(number("MHF_SHADOW_MIN_ELEVATION", shadow_min_elevation_), 5.0f, 90.0f);
    shadow_size_ = static_cast<GLsizei>(std::clamp(static_cast<int>(number("MHF_SHADOW_MAP", 8192.0f)), 512, 8192));
    shadow_map_program_ = compile(kShadowMapVertex, kShadowMapFragment);
    shadow_factor_program_ = compile(kShadowQuadVertex, kShadowFactorFragment);
    shadow_apply_program_ = compile(kShadowQuadVertex, kShadowApplyFragment);
    shadow_show_program_ = compile(kShadowShowVertex, kShadowShowFragment);
    shadow_facing_program_ = compile(kShadowFacingVertex, kShadowFacingFragment);
    shadow_ready_ = shadow_facing_program_ != 0 && shadow_map_program_ != 0 && shadow_factor_program_ != 0 && shadow_apply_program_ != 0 && shadow_show_program_ != 0;
    if (shadow_ready_) {
        gl_.ActiveTexture(GL_TEXTURE0);
        for (int i = 0; i < 2; ++i) {
            gl_.GenTextures(1, &shadow_color_[i]);
            gl_.BindTexture(GL_TEXTURE_2D, shadow_color_[i]);
            gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_R8, shadow_size_, shadow_size_, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
            set_texture_params(false, true, true);
            gl_.GenTextures(1, &shadow_depth_[i]);
            gl_.BindTexture(GL_TEXTURE_2D, shadow_depth_[i]);
            gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, shadow_size_, shadow_size_, 0, GL_DEPTH_COMPONENT,
                           GL_UNSIGNED_INT, nullptr);
            set_texture_params(false, true, true);
            gl_.GenFramebuffers(1, &shadow_fbo_[i]);
            gl_.BindFramebuffer(GL_FRAMEBUFFER, shadow_fbo_[i]);
            gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shadow_color_[i], 0);
            gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow_depth_[i], 0);
            if (gl_.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) shadow_ready_ = false;
        }
        gl_.GenTextures(1, &shadow_player_color_);
        gl_.BindTexture(GL_TEXTURE_2D, shadow_player_color_);
        gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_R8, kShadowPlayerSize, kShadowPlayerSize, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        set_texture_params(false, true, true);
        gl_.GenTextures(1, &shadow_player_depth_);
        gl_.BindTexture(GL_TEXTURE_2D, shadow_player_depth_);
        gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kShadowPlayerSize, kShadowPlayerSize, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        set_texture_params(false, true, true);
        gl_.GenFramebuffers(1, &shadow_player_fbo_);
        gl_.BindFramebuffer(GL_FRAMEBUFFER, shadow_player_fbo_);
        gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shadow_player_color_, 0);
        gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow_player_depth_, 0);
        if (gl_.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) shadow_ready_ = false;
        gl_.BindTexture(GL_TEXTURE_2D, 0);
        gl_.GenVertexArrays(1, &shadow_vao_);
        gl_.GenBuffers(1, &shadow_vbo_);
        gl_.BindVertexArray(shadow_vao_);
        gl_.BindBuffer(GL_ARRAY_BUFFER, shadow_vbo_);
        gl_.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
        gl_.EnableVertexAttribArray(0);
        gl_.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void *>(3 * sizeof(float)));
        gl_.EnableVertexAttribArray(1);
        for (int i = 0; i < 2; ++i) {
            gl_.GenVertexArrays(1, &shadow_facing_vao_[i]);
            gl_.GenBuffers(1, &shadow_facing_vbo_[i]);
            gl_.BindVertexArray(shadow_facing_vao_[i]);
            gl_.BindBuffer(GL_ARRAY_BUFFER, shadow_facing_vbo_[i]);
            gl_.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
            gl_.EnableVertexAttribArray(0);
            gl_.VertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void *>(3 * sizeof(float)));
            gl_.EnableVertexAttribArray(1);
            gl_.VertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void *>(4 * sizeof(float)));
            gl_.EnableVertexAttribArray(2);
        }
        gl_.BindVertexArray(empty_vao_);
    }
    std::fprintf(stderr, "[shadow] %s: map %d, reach %g\n", shadow_ready_ ? "on" : "failed", static_cast<int>(shadow_size_), shadow_reach_);
    return shadow_ready_;
}

// A query's answer arrives some frames later, and a slot still waiting for it skips a turn.
void GlBackend::shadow_time_begin(int slot) {
    ShadowTime &t = shadow_time_[slot];
    if (t.query == 0) gl_.GenQueries(1, &t.query);
    if (t.pending) {
        GLuint ready = 0;
        gl_.GetQueryObjectuiv(t.query, GL_QUERY_RESULT_AVAILABLE, &ready);
        if (ready == 0) return;
        GLuint64 ns = 0;
        gl_.GetQueryObjectui64v(t.query, GL_QUERY_RESULT, &ns);
        t.ms += static_cast<double>(ns) / 1e6;
        ++t.count;
        t.pending = false;
    }
    gl_.BeginQuery(GL_TIME_ELAPSED, t.query);
    shadow_time_now_ = slot;
}

void GlBackend::shadow_time_end() {
    if (shadow_time_now_ < 0) return;
    gl_.EndQuery(GL_TIME_ELAPSED);
    shadow_time_[shadow_time_now_].pending = true;
    shadow_time_now_ = -1;
}

void GlBackend::shadow_forget_scenery() {
    shadow_seen_vertices_.clear();
    shadow_seen_ranges_.clear();
    shadow_seen_keys_.clear();
    shadow_seen_anchor_.clear();
    shadow_seen_built_ = false;
    shadow_seen_grew_ = false;
    shadow_seen_doubts_ = 0;
    shadow_known_.clear();
}

void GlBackend::run_shadow(RenderTarget &rt) {
    const auto cpu_start = std::chrono::steady_clock::now();
    // The size of the maps, when the menu changes it. The kept scenery's map changes too, and is drawn again.
    if (const int quality = live_->shadow_quality.load(std::memory_order_relaxed); quality != shadow_quality_used_ && shadow_ready_) {
        shadow_quality_used_ = quality;
        shadow_size_ = 2048 << std::clamp(quality, 0, 2);
        gl_.ActiveTexture(GL_TEXTURE0);
        const auto resize = [&](GLuint color, GLuint depth) {
            gl_.BindTexture(GL_TEXTURE_2D, color);
            gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_R8, shadow_size_, shadow_size_, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
            gl_.BindTexture(GL_TEXTURE_2D, depth);
            gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, shadow_size_, shadow_size_, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        };
        for (int i = 0; i < 2; ++i) resize(shadow_color_[i], shadow_depth_[i]);
        if (shadow_seen_fbo_ != 0) resize(shadow_seen_color_, shadow_seen_depth_);
        gl_.BindTexture(GL_TEXTURE_2D, 0);
        shadow_seen_built_ = false;
    }
    if (shadow_capture_frame_ != frame_ || shadow_draws_.empty() || !shadow_setup() || shadow_vz_[0] == 0.0f) {
        shadow_pass_.valid = false;
        return;
    }
    // The sliders go up to the whole map, but past this the scenery would pass for one object and skip every rule made for it.
    const float largest = std::min(live_->shadow_max_object.load(std::memory_order_relaxed), 1000.0f);
    const float largest_piece = std::min(live_->shadow_max_piece.load(std::memory_order_relaxed), 1000.0f);
    bool whole_map = live_->shadow_whole_map.load(std::memory_order_relaxed);
    const auto casts = [&](const ShadowGroup &g) { return g.skinned || g.size() <= largest; };

    // The scenery is the biggest group that is not skinned: its Y axis is the world's up. The hunter is the nearest skinned
    // group: the map is centred on it, so the sharpest shadows are where the player looks.
    const ShadowGroup *stage = nullptr, *hunter = nullptr, *fallback = nullptr;
    float hunter_distance = 1e30f;
    const auto centre_of = [](const ShadowGroup &g, float *out) {
        for (int k = 0; k < 3; ++k) out[k] = 0.5f * (g.eye_low[k] + g.eye_high[k]);
    };
    for (const ShadowGroup &g : shadow_groups_) {
        if (g.triangles == 0) continue;
        if (!g.skinned && (stage == nullptr || g.triangles > stage->triangles)) stage = &g;
        float c[3];
        centre_of(g, c);
        const float distance = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
        // The player's model is the most detailed of the people on screen (monsters are bigger; villagers, even close to the
        // camera, have about half its triangles).
        if (g.skinned && g.size() <= 300.0f && (hunter == nullptr || g.triangles > hunter->triangles)) hunter = &g;
        if (g.skinned && distance < hunter_distance) {
            hunter_distance = distance;
            if (fallback == nullptr || distance <= hunter_distance) fallback = &g;
        }
    }
    if (hunter == nullptr) hunter = fallback; // only something big is around: the nearest
    float center[3] = {0.0f, 0.0f, -shadow_reach_ * 0.5f};
    if (hunter != nullptr) centre_of(*hunter, center);

    // The sun: the game's light, kept above the horizon so that shadows do not stretch across the whole map.
    const auto normalize = [](float *v) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l < 1e-6f) return false;
        for (int k = 0; k < 3; ++k) v[k] /= l;
        return true;
    };
    const auto cross = [](const float *x, const float *y, float *out) {
        out[0] = x[1] * y[2] - x[2] * y[1];
        out[1] = x[2] * y[0] - x[0] * y[2];
        out[2] = x[0] * y[1] - x[1] * y[0];
    };
    float up_axis[3] = {0.0f, 1.0f, 0.0f};
    if (stage != nullptr) {
        const auto &v = stage->view;
        const float *a = stage->axis[1];
        up_axis[0] = v[0] * a[0] + v[3] * a[1] + v[6] * a[2];
        up_axis[1] = v[1] * a[0] + v[4] * a[1] + v[7] * a[2];
        up_axis[2] = v[2] * a[0] + v[5] * a[1] + v[8] * a[2];
        normalize(up_axis);
    }
    float dir[3] = {up_axis[0] * 0.8f + 0.6f, up_axis[1] * 0.8f, up_axis[2] * 0.8f};
    if (shadow_light_ok_) std::copy(shadow_light_, shadow_light_ + 3, dir);
    if (!normalize(dir)) return;
    const float game_height = dir[0] * up_axis[0] + dir[1] * up_axis[1] + dir[2] * up_axis[2];
    // Night: the fog goes dark on the hunting grounds. Indoors in the village it is dark too, by day as well.
    if (shadow_light_ok_ && shadow_zone_ >= 0) {
        const std::uint32_t fog = shadow_light_colours_[3];
        const bool dark_fog = std::max({fog & 0xFFu, fog >> 8u & 0xFFu, fog >> 16u & 0xFFu}) <= 0x44u;
        const std::string &zone_name = shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)];
        // Closed zones have the same fog by day and by night: the time stays what it was in the zone before.
        const bool says_nothing = zone_name == "areas/hunting/desert/area4" || zone_name == "areas/hunting/desert/area6" || zone_name == "areas/hunting/desert/area10" || zone_name == "areas/hunting/jungle/area6" || zone_name == "areas/hunting/jungle/area7" || zone_name == "areas/hunting/jungle/area8" || zone_name == "areas/hunting/jungle/area9";
        // A new zone's fog comes a moment before the zone is known, while the old one is still named: it has to last.
        const bool hunting = zone_name.rfind("areas/hunting/", 0) == 0;
        const bool camp = zone_name.size() >= 5u && zone_name.compare(zone_name.size() - 5u, 5u, "/base") == 0;
        if (!hunting) shadow_night_settled_ = false;
        const bool night_now = dark_fog && hunting;
        shadow_night_pictures_ = !says_nothing && !(shadow_night_settled_ && !camp) && night_now != shadow_night_ ? shadow_night_pictures_ + 1 : 0;
        if (shadow_night_pictures_ >= 60) {
            shadow_night_ = night_now;
            shadow_night_pictures_ = 0;
        }
        // The camp's fog says for the whole map; elsewhere only until the camp has been seen, as when a hunt starts away from it.
        if (camp && shadow_night_pictures_ == 0) shadow_night_settled_ = true;
    }
    // Where the game's own light stands in the scenery's frame.
    if (stage != nullptr) {
        const auto &v = stage->view;
        const float *a = stage->axis[0], *c = stage->axis[2];
        const float east[3] = {v[0] * a[0] + v[3] * a[1] + v[6] * a[2], v[1] * a[0] + v[4] * a[1] + v[7] * a[2], v[2] * a[0] + v[5] * a[1] + v[8] * a[2]};
        const float south[3] = {v[0] * c[0] + v[3] * c[1] + v[6] * c[2], v[1] * c[0] + v[4] * c[1] + v[7] * c[2], v[2] * c[0] + v[5] * c[1] + v[8] * c[2]};
        // The fortress's zone 6, the old swamp's zones 5 and 10 (its zone 6 is taken for 10) and most of the old volcano: the game's light
        // stands straight up, or straight down, its bearing shakes from picture to picture and the map with it. A bearing of their own
        // there, a hair off straight up, that holds still.
        static constexpr const char *kStraightUp[] = {"areas/hunting/fortress/area6",    "areas/hunting/old_swamp/area5",
                                                      "areas/hunting/old_swamp/area10",  "areas/hunting/old_volcano/base",
                                                      "areas/hunting/old_volcano/area1", "areas/hunting/old_volcano/area3",
                                                      "areas/hunting/old_volcano/area4", "areas/hunting/old_volcano/area5",
                                                      "areas/hunting/old_volcano/area8"};
        if (shadow_zone_ >= 0 && std::find(std::begin(kStraightUp), std::end(kStraightUp), shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)]) != std::end(kStraightUp)) {
            const float along = south[0] * up_axis[0] + south[1] * up_axis[1] + south[2] * up_axis[2];
            float flat[3] = {south[0] - along * up_axis[0], south[1] - along * up_axis[1], south[2] - along * up_axis[2]};
            if (normalize(flat)) {
                const float lift = std::sin(89.0f * 0.0174533f), across = std::cos(89.0f * 0.0174533f);
                for (int k = 0; k < 3; ++k) dir[k] = flat[k] * across + up_axis[k] * lift;
            }
        }
        const float bearing = std::atan2(dir[0] * east[0] + dir[1] * east[1] + dir[2] * east[2], dir[0] * south[0] + dir[1] * south[1] + dir[2] * south[2]) * 57.29578f;
        static std::uint32_t told_colours[4] = {};
        if (shadow_light_ok_ && !std::equal(told_colours, told_colours + 4, shadow_light_colours_)) {
            std::copy(shadow_light_colours_, shadow_light_colours_ + 4, told_colours);
            std::fprintf(stderr, "[shadow colours] zone %s: ambient %06x, light ambient %06x, light diffuse %06x, fog %06x\n", shadow_zone_ >= 0 ? shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)].c_str() : "none",
                         told_colours[0], told_colours[1], told_colours[2], told_colours[3]);
        }
        static float told_bearing = 1000.0f;
        static int told_zone = -2;
        if (shadow_light_ok_ && (std::fabs(bearing - told_bearing) > 3.0f || told_zone != shadow_zone_)) {
            told_bearing = bearing;
            told_zone = shadow_zone_;
            std::fprintf(stderr, "[shadow sun] zone %s: the game's light stands at %.1f degrees, %.1f high\n", shadow_zone_ >= 0 ? shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)].c_str() : "none", bearing, std::asin(std::clamp(game_height, -1.0f, 1.0f)) * 57.29578f);
        }
    }
    const float height = dir[0] * up_axis[0] + dir[1] * up_axis[1] + dir[2] * up_axis[2];
    // The slider, for trying heights out, goes over what the game's list says for the zone.
    float sun_height = live_->shadow_sun_height.load(std::memory_order_relaxed);
    float zone_darkness = -1.0f;
    float sun_turn = live_->shadow_sun_turn.load(std::memory_order_relaxed);
    if (shadow_zone_ >= 0) {
        const std::string zone = (shadow_night_ ? "night:" : "") + shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)];
        for (const auto &[name, sun] : live_->shadow_zone_suns) {
            if (name != zone) continue;
            if (sun_height < 0.0f) sun_height = sun[0];
            zone_darkness = sun[1];
            if (sun_turn <= 0.0f) sun_turn = sun[2];
        }
    }
    // A height asked for is taken as it is, low or high. Otherwise the game's, kept from lying too low.
    const bool sun_placed = sun_height >= 0.0f;
    const float lowest = std::sin((sun_placed ? sun_height : shadow_min_elevation_) * 0.0174533f);
    if (sun_placed || height < lowest) {
        float flat[3] = {dir[0] - height * up_axis[0], dir[1] - height * up_axis[1], dir[2] - height * up_axis[2]};
        if (normalize(flat)) {
            const float across = std::sqrt(1.0f - lowest * lowest);
            for (int k = 0; k < 3; ++k) dir[k] = flat[k] * across + up_axis[k] * lowest;
        } else {
            std::copy(up_axis, up_axis + 3, dir);
        }
    }
    // Around the world's up: its height stays.
    if (sun_turn > 0.0f) {
        const float angle = sun_turn * 0.0174533f, c = std::cos(angle), s = std::sin(angle);
        const float along = dir[0] * up_axis[0] + dir[1] * up_axis[1] + dir[2] * up_axis[2];
        float side[3];
        cross(up_axis, dir, side);
        for (int k = 0; k < 3; ++k) dir[k] = dir[k] * c + side[k] * s + up_axis[k] * along * (1.0f - c);
        normalize(dir);
    }
    // The map's axes follow the world (its up and the sun), not the camera, so that shadows do not swim when the camera turns.
    float right[3], up[3];
    cross(up_axis, dir, right);
    if (!normalize(right)) {
        const float helper[3] = {std::fabs(dir[1]) < 0.9f ? 0.0f : 1.0f, std::fabs(dir[1]) < 0.9f ? 1.0f : 0.0f, 0.0f};
        cross(helper, dir, right);
        normalize(right);
    }
    cross(dir, right, up);
    // On whole texels of the map, so that shadows of still objects do not swim while the hunter walks.
    const float texel = 2.0f * shadow_reach_ / static_cast<float>(shadow_size_);
    float along_right = 0.0f, along_up = 0.0f, along_dir = 0.0f, world_right = 0.0f, world_up = 0.0f;
    float world_origin[3] = {};
    if (stage != nullptr) { // the point of the world that stays put: the grid of texels hangs from it
        const auto &v = stage->view;
        const auto &w = stage->world;
        for (int k = 0; k < 3; ++k) world_origin[k] = v[k] * w[9] + v[3 + k] * w[10] + v[6 + k] * w[11] + v[9 + k];
    }
    for (int k = 0; k < 3; ++k) {
        along_right += center[k] * right[k];
        along_up += center[k] * up[k];
        along_dir += center[k] * dir[k];
        world_right += world_origin[k] * right[k];
        world_up += world_origin[k] * up[k];
    }
    along_right = std::floor((along_right - world_right) / texel) * texel + world_right;
    along_up = std::floor((along_up - world_up) / texel) * texel + world_up;
    for (int k = 0; k < 3; ++k) center[k] = right[k] * along_right + up[k] * along_up + dir[k] * along_dir;

    // Plants and trees are often drawn with the scenery's own matrix, so the scenery's group is looked at piece by piece.
    // A solid draw counts when it is small and slender (a trunk, a post): the terrain is cut into small draws too, but those
    // are wide. A cut-out counts
    // triangle by triangle, because one draw can hold the plants of a whole area; huge or lying ones are overlays on the ground.
    // The sky and the backdrops are huge objects of very few triangles (measured: 40 to 480 over 65000 to 98000 units, when the
    // terrain has about 9000), so they neither cast nor shade. The mountains far around the map stay out of the map of shadows:
    // nothing farther from the hunter than the map is wide casts.
    const auto backdrop = [](const ShadowGroup &g) {
        return !g.skinned && g.size() > 5000.0f && static_cast<float>(g.triangles) < 0.02f * g.size();
    };
    // A draw that encloses the camera is a shell of the sky or of the air (a sandstorm): it does not depend on how many triangles
    // it has. The same goes for a single huge draw of very few triangles.
    const auto shell_by_shape = [&](const ShadowDraw &d) {
        if (d.size() <= 5000.0f) return false;
        if (static_cast<float>(d.count / 3u) < 0.01f * d.size()) return true;
        const float *x = shadow_vertices_.data() + static_cast<std::size_t>(d.first) * 5u;
        float low[3] = {x[0], x[1], x[2]}, high[3] = {x[0], x[1], x[2]};
        for (std::uint32_t v = 1; v < d.count; ++v) {
            const float *p = x + static_cast<std::size_t>(v) * 5u;
            for (int k = 0; k < 3; ++k) {
                low[k] = std::min(low[k], p[k]);
                high[k] = std::max(high[k], p[k]);
            }
        }
        return low[0] < 0.0f && high[0] > 0.0f && low[1] < 0.0f && high[1] > 0.0f && low[2] < 0.0f && high[2] > 0.0f;
    };
    // Mist and fog in the air: what the game's list names, and textures that are mostly clear. Rock can be a single opaque
    // triangle too, so no shape tells them apart.
    // Zones 6 and 8 of the volcano: what only adds light is mist, however opaque it comes near as it drifts, or the glow in the
    // sky around the peak, cut into pieces that do not look like a shell.
    const bool adding_light_is_air = shadow_zone_ >= 0 && (shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/volcano/area6" ||
                                                           shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/volcano/area8");
    // The jungle's camp: the mountain over its waterfall is drawn blended and tall, and the waterfall's water fades to nothing at
    // its edges. Air there: they neither cast nor take the shade. Not what adds light over the ground.
    if (kJungleCampWater && shadow_zone_ >= 0 && shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/jungle/base") {
        for (ShadowDraw &d : shadow_draws_) {
            if (d.listed == 0 && d.blended && !d.cutout && !shadow_groups_[d.group].skinned && d.paint[0] != 0xA2u &&
                (d.height() >= 1500.0f || d.alpha_low < 16.0f)) {
                d.listed = 1;
            }
        }
    }
    const auto air = [adding_light_is_air](const ShadowDraw &d) {
        if (d.listed == 1 || (adding_light_is_air && d.paint[0] == 0xA2u)) return true;
        // Fading out along their own vertices is what shapes floating in the air do; rock stays opaque. Leaves are left to their cut-outs.
        const bool fades = d.blended && !d.cutout && d.holes >= 0.02f && d.alpha_low < 200.0f;
        // See-through all over, like the haze some maps hang right in front of the camera.
        const bool thin = d.blended && !d.cutout && d.alpha_high < 200.0f;
        return d.listed < 2 && (d.translucent || fades || thin);
    };
    // The ground: flat pieces overlap a little, with thin upright strips along their seams, and each would cast a thin line on
    // the next. So what lies low and is flat or a strip does not cast. High up the same shapes are ledges and tops of rock,
    // which must, or their shadows have holes. Low means up to a little above the hunter's feet.
    const auto height_of = [&](const float *p) { return p[0] * up_axis[0] + p[1] * up_axis[1] + p[2] * up_axis[2]; };
    float feet = height_of(center) - 200.0f;
    if (hunter != nullptr) {
        feet = 1e30f;
        for (int c = 0; c < 8; ++c) {
            const float corner[3] = {(c & 1) != 0 ? hunter->eye_low[0] : hunter->eye_high[0], (c & 2) != 0 ? hunter->eye_low[1] : hunter->eye_high[1],
                                     (c & 4) != 0 ? hunter->eye_low[2] : hunter->eye_high[2]};
            feet = std::min(feet, height_of(corner));
        }
    }
    // Fixed for the whole zone, as a height in the world: following the hunter, the same walls would change sides as he walks up
    // and down a slope, and buildings would change shade.
    if (shadow_zone_ >= 0) {
        const float origin_height = height_of(world_origin);
        // Zone 2 of the snowy mountains has two floors far apart: walking up to the other, its ground is taken again (and what
        // was kept of the scenery, measured from the old one, goes). A slope never comes near that much.
        const bool two_floors = shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/snowy_mountains/area2";
        const bool other_floor = two_floors && shadow_floor_zone_ == shadow_zone_ && hunter != nullptr &&
                                 std::fabs(feet - origin_height - shadow_floor_) > 500.0f;
        if (shadow_floor_zone_ != shadow_zone_ || other_floor) {
            if (other_floor) shadow_forget_scenery();
            shadow_floor_zone_ = shadow_zone_;
            shadow_floor_ = feet - origin_height;
        }
        feet = shadow_floor_ + origin_height;
    }
    // The sky is cut into pieces, and those too small to look like a shell have the texture of the ones that do. High up, that
    // says what they are: a single triangle of sky over a cave is otherwise just like one of rock.
    std::unordered_set<std::uint64_t> sky_textures;
    for (const ShadowDraw &d : shadow_draws_) {
        if (d.count != 0 && d.listed != 2 && !shadow_groups_[d.group].skinned && shell_by_shape(d)) sky_textures.insert(d.texture_hash);
    }
    // The painted background: where the scenery has fog, the game leaves it out here, the distance being in the picture already.
    // Water goes without too, but it is drawn with another alpha test. Pieces of the background match what is around them, and
    // only a shadow would give them away.
    std::uint32_t with_fog = 0u, without_fog = 0u;
    for (const ShadowDraw &d : shadow_draws_) {
        if (d.count == 0 || shadow_groups_[d.group].skinned) continue;
        ((d.paint[6] & 2u) != 0 ? with_fog : without_fog) += d.count / 3u;
    }
    const bool scenery_has_fog = with_fog > 4u * without_fog;
    const bool desert_walls = shadow_zone_ >= 0 && (shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/desert/area3" ||
                                                                     shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/desert/area7" ||
                                                                     shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/desert/area9");
    // The jungle's zone 7 by day: its white walls.
    const bool jungle_walls = !shadow_night_ && shadow_zone_ >= 0 && shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/jungle/area7";
    const bool walls_look_like_sky = desert_walls || jungle_walls || shadow_zone_ >= 0 && (shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/volcano/area3" ||
                                                           shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/tower_1_2/area1" ||
                                                           shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/old_jungle/area8" ||
                                                           shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/volcano/area5" ||
                                                           shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/volcano/area6" ||
                                                           shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/forestandhills/area6");
    const bool air_reaches_ground = shadow_zone_ >= 0 && (shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/jungle/area8" ||
                                                          shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/old_jungle/area7" ||
                                                          shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/volcano/area8");
    const auto background = [&](const ShadowDraw &d) {
        if (!scenery_has_fog || (d.paint[6] & 3u) != 1u || (d.paint[4] & 0xFFFFu) != 0x8007u || d.listed == 2 || d.size() < 500.0f || shadow_groups_[d.group].skinned) return false;
        // The bed of the sea goes without fog as well, and that lies under the hunter's feet.
        for (std::uint32_t v = d.first; v < d.first + d.count; ++v) {
            if (height_of(shadow_vertices_.data() + static_cast<std::size_t>(v) * 5u) >= feet) return true;
        }
        return false;
    };
    const auto shell = [&](const ShadowDraw &d) {
        if (walls_look_like_sky) return false; // walls, however big: they take the sun as any other
        if (shell_by_shape(d) || background(d)) return true;
        if (d.listed == 2 || sky_textures.count(d.texture_hash) == 0) return false;
        for (std::uint32_t v = d.first; v < d.first + d.count; ++v) {
            if (height_of(shadow_vertices_.data() + static_cast<std::size_t>(v) * 5u) < feet + 1000.0f) return false;
        }
        return true;
    };
    const auto strip_or_flat = [&](const float *t) { // 0: neither, 1: a seam, 2: lying flat
        const float e0[3] = {t[5] - t[0], t[6] - t[1], t[7] - t[2]}, e1[3] = {t[10] - t[0], t[11] - t[1], t[12] - t[2]};
        float n[3];
        cross(e0, e1, n);
        const float twice_area = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!normalize(n)) return 0;
        const float upright = std::fabs(n[0] * up_axis[0] + n[1] * up_axis[1] + n[2] * up_axis[2]);
        if (upright > 0.85f) return 2;
        float longest = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float *p = t + 5 * i, *q = t + 5 * ((i + 1) % 3);
            longest = std::max(longest, (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2]));
        }
        // A seam is thin whichever way it lies: measured across, not by how much it rises, which a row of a low wall shares. Or
        // long for how thin it is, like the strips along the sand: those of a wall are nowhere near that stretched.
        const float length = std::max(std::sqrt(longest), 1.0f), across = twice_area / length;
        return upright < 0.5f && (across < 40.0f || across < 0.12f * length) ? 1 : 0;
    };
    // The map's hidden walls: upright sheets that go down under the sand, seen only where the ground cuts them. Shading one would
    // draw a line along that cut. Rock stands on the ground: these reach well below the hunter's feet.
    // Only zones 2, 5 and 7 of the desert and zone 2 of the snowy mountains have them. Elsewhere a low upright wall seen from above is a cliff (the forest and hills' zone 6
    // has white ones), and takes the side shade.
    const bool buried_walls_here = shadow_zone_ >= 0 && (shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/desert/area2" ||
                                                         shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/desert/area5" ||
                                                         shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/desert/area7" ||
                                                         shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/snowy_mountains/area2");
    const auto buried_wall = [&](const float *t) {
        if (!buried_walls_here) return false;
        const float e0[3] = {t[5] - t[0], t[6] - t[1], t[7] - t[2]}, e1[3] = {t[10] - t[0], t[11] - t[1], t[12] - t[2]};
        float n[3];
        cross(e0, e1, n);
        return normalize(n) && std::fabs(n[0] * up_axis[0] + n[1] * up_axis[1] + n[2] * up_axis[2]) < 0.35f &&
               std::max({height_of(t), height_of(t + 5), height_of(t + 10)}) < feet + 400.0f &&
               std::min({height_of(t), height_of(t + 5), height_of(t + 10)}) < feet - 150.0f;
    };
    // Sand and gentle slopes low down: the game's own shading stays, and layers of ground over them would cut any shade drawn here.
    const auto low_ground = [&](const float *t) {
        return std::max({height_of(t), height_of(t + 5), height_of(t + 10)}) < feet + 400.0f;
    };
    const float cast_limit = 18000.0f; // a fixed distance: the backdrops around the map begin about here
    const auto near_enough = [&](const ShadowDraw &d) {
        const float *x = shadow_vertices_.data() + static_cast<std::size_t>(d.first) * 5u;
        float low[3] = {x[0], x[1], x[2]}, high[3] = {x[0], x[1], x[2]};
        for (std::uint32_t v = 1; v < d.count; ++v) {
            const float *p = x + static_cast<std::size_t>(v) * 5u;
            for (int k = 0; k < 3; ++k) {
                low[k] = std::min(low[k], p[k]);
                high[k] = std::max(high[k], p[k]);
            }
        }
        float gap = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float g = std::max({low[k] - center[k], center[k] - high[k], 0.0f});
            gap += g * g;
        }
        return gap <= cast_limit * cast_limit;
    };
    // Inside a cave the sun does not count, and only objects cast. The game's list of textures says where that is. For a cave not
    // on the list yet MHF_SHADOW_CAVES=1 looks for rock overhead all around the hunter, not just on one side as under an arch or
    // next to a cliff: a few times a second, and it has to agree three times in a row.
    static const bool cave_check = [] {
        const char *e = std::getenv("MHF_SHADOW_CAVES");
        return e != nullptr && e[0] != '0';
    }();
    bool cave_textures = false;
    for (const ShadowDraw &d : shadow_draws_) cave_textures = cave_textures || (d.listed == 3 && d.count != 0 && d.target == rt.address);
    if (cave_textures != shadow_cave_textures_) {
        shadow_cave_textures_ = cave_textures;
        std::fprintf(stderr, "[shadow] %s a cave, by its textures: whole map shadows %s\n", cave_textures ? "in" : "out of", cave_textures ? "off" : "on");
    }
    if (cave_check && whole_map && hunter != nullptr && shadow_frame_count_++ % 10u == 0) {
        float side[3];
        cross(up_axis, right, side);
        float hunter_centre[3];
        centre_of(*hunter, hunter_centre);
        const float reach_out = 400.0f;
        float points[9][2];
        const float cx = hunter_centre[0] * right[0] + hunter_centre[1] * right[1] + hunter_centre[2] * right[2];
        const float cy = hunter_centre[0] * side[0] + hunter_centre[1] * side[1] + hunter_centre[2] * side[2];
        points[0][0] = cx;
        points[0][1] = cy;
        for (int i = 0; i < 8; ++i) {
            points[i + 1][0] = cx + reach_out * std::cos(0.7853982f * static_cast<float>(i));
            points[i + 1][1] = cy + reach_out * std::sin(0.7853982f * static_cast<float>(i));
        }
        unsigned covered = 0;
        for (const ShadowDraw &d : shadow_draws_) {
            if (d.count == 0 || d.target != rt.address || d.cutout || air(d)) continue;
            const ShadowGroup &g = shadow_groups_[d.group];
            if (g.skinned || backdrop(g) || shell(d)) continue;
            for (std::uint32_t v = 0; v + 3u <= d.count; v += 3u) {
                const float *t = shadow_vertices_.data() + (static_cast<std::size_t>(d.first) + v) * 5u;
                if (std::min({height_of(t), height_of(t + 5), height_of(t + 10)}) < feet + 200.0f) continue;
                float x[3], y[3];
                for (int k = 0; k < 3; ++k) {
                    const float *p = t + 5 * k;
                    x[k] = p[0] * right[0] + p[1] * right[1] + p[2] * right[2];
                    y[k] = p[0] * side[0] + p[1] * side[1] + p[2] * side[2];
                }
                for (unsigned i = 0; i < 9u; ++i) {
                    if ((covered & (1u << i)) != 0) continue;
                    const float px = points[i][0], py = points[i][1];
                    const float a = (x[1] - x[0]) * (py - y[0]) - (y[1] - y[0]) * (px - x[0]);
                    const float b = (x[2] - x[1]) * (py - y[1]) - (y[2] - y[1]) * (px - x[1]);
                    const float c = (x[0] - x[2]) * (py - y[2]) - (y[0] - y[2]) * (px - x[2]);
                    if ((a >= 0.0f && b >= 0.0f && c >= 0.0f) || (a <= 0.0f && b <= 0.0f && c <= 0.0f)) covered |= 1u << i;
                }
            }
        }
        int count = 0;
        for (unsigned i = 0; i < 9u; ++i) count += (covered >> i) & 1u;
        const bool under_rock = count >= 8;
        shadow_cave_votes_ = under_rock == shadow_in_cave_ ? 0 : shadow_cave_votes_ + 1;
        if (shadow_cave_votes_ >= 3) {
            shadow_in_cave_ = under_rock;
            shadow_cave_votes_ = 0;
            std::fprintf(stderr, "[shadow] %s a cave (%d of 9 points under rock): whole map shadows %s\n", under_rock ? "in" : "out of", count, under_rock ? "off" : "on");
        }
    }
    const auto scene_now = std::chrono::steady_clock::now();
    const bool after_a_pause = scene_now - shadow_last_scene_ > std::chrono::milliseconds(150);
    shadow_last_scene_ = scene_now;
    // Where the game is: the zone with the largest share of its own textures on the screen, once that has held for a while. Not
    // the one with the most triangles: a zone with few textures of its own loses to a neighbour it shares its pieces with.
    shadow_zone_seen_.clear();
    for (const ShadowDraw &d : shadow_draws_) {
        if (d.zone >= 0 && d.target == rt.address) shadow_zone_seen_.emplace_back(d.zone, d.texture_hash);
    }
    std::sort(shadow_zone_seen_.begin(), shadow_zone_seen_.end());
    shadow_zone_seen_.erase(std::unique(shadow_zone_seen_.begin(), shadow_zone_seen_.end()), shadow_zone_seen_.end());
    shadow_zone_triangles_.assign(shadow_zone_names_.size(), 0u);
    for (const auto &[zone, hash] : shadow_zone_seen_) ++shadow_zone_triangles_[static_cast<std::size_t>(zone)];
    // A map's "base" folder may also hold what its zones share, so it only counts when nothing of a numbered zone is on the screen.
    int zone_now = -1;
    float zone_share = 0.0f;
    for (int pass = 0; pass < 2 && zone_now < 0; ++pass) {
        for (std::size_t i = 0; i < shadow_zone_triangles_.size(); ++i) {
            const std::string &name = shadow_zone_names_[i];
            const bool base = name.size() > 5u && name.compare(name.size() - 5u, 5u, "/base") == 0;
            if (base != (pass == 1) || shadow_zone_triangles_[i] == 0u) continue;
            const float share = static_cast<float>(shadow_zone_triangles_[i]) / static_cast<float>(std::max(1, shadow_zone_totals_[i]));
            if (share > zone_share) {
                zone_share = share;
                zone_now = static_cast<int>(i);
            }
        }
    }
    // The camps' textures show in the zones around them, so a numbered zone comes first. Not a stray texture of another map, though:
    // the jungle's camp, with 23 of its 25, shows 2 of the desert's zone 1.
    if (zone_now >= 0 && zone_share < 0.3f) {
        for (std::size_t i = 0; i < shadow_zone_triangles_.size(); ++i) {
            const std::string &name = shadow_zone_names_[i];
            const bool base = name.size() > 5u && name.compare(name.size() - 5u, 5u, "/base") == 0;
            const float share = static_cast<float>(shadow_zone_triangles_[i]) / static_cast<float>(std::max(1, shadow_zone_totals_[i]));
            if (base && share >= 0.5f && share > zone_share) {
                zone_share = share;
                zone_now = static_cast<int>(i);
            }
        }
    }
    // Caves of one map share textures, and the pack keeps those with one of them, so a zone can look like a poor match for another:
    // the swamp's zone 7 shows 9 of 14 of zone 3's. It does show a good part of its own, though, which zone 3 itself never does.
    if (zone_now >= 0 && zone_share < 0.8f && shadow_zone_names_[static_cast<std::size_t>(zone_now)].find("/hunting/") != std::string::npos) {
        const std::string &best = shadow_zone_names_[static_cast<std::size_t>(zone_now)];
        const std::string map = best.substr(0, best.rfind('/') + 1u);
        float rival_share = 0.0f;
        int rival = -1;
        for (std::size_t i = 0; i < shadow_zone_triangles_.size(); ++i) {
            const std::string &name = shadow_zone_names_[i];
            if (static_cast<int>(i) == zone_now || name.compare(0, map.size(), map) != 0 || name.find("/base") != std::string::npos) continue;
            const float share = static_cast<float>(shadow_zone_triangles_[i]) / static_cast<float>(std::max(1, shadow_zone_totals_[i]));
            // Only a zone much smaller than the winner: those are the ones the pack left with few textures of their own.
            if (shadow_zone_triangles_[i] >= 2u && share >= 0.4f && share > rival_share &&
                2 * shadow_zone_totals_[i] <= shadow_zone_totals_[static_cast<std::size_t>(zone_now)]) {
                rival_share = share;
                rival = static_cast<int>(i);
            }
        }
        if (rival >= 0) {
            zone_now = rival;
            zone_share = rival_share;
        }
    }
    if (zone_now == shadow_zone_) {
        shadow_zone_votes_ = 0;
    } else if (zone_now >= 0) {
        shadow_zone_votes_ = zone_now == shadow_zone_next_ ? shadow_zone_votes_ + 1 : 1;
        shadow_zone_next_ = zone_now;
        // Zones load behind a moment with no 3D at all: what comes after it is a new zone, and showing the old one's shadows
        // until it is sure would flash. While the game plays on, it has to hold for a while.
        if (shadow_zone_votes_ >= 15 || after_a_pause) {
            shadow_zone_ = zone_now;
            shadow_zone_votes_ = 0;
            std::fprintf(stderr, "[shadow] zone %s (%.0f%% of its textures)\n", shadow_zone_names_[static_cast<std::size_t>(zone_now)].c_str(), zone_share * 100.0f);
            // What else was on the screen: zones that share textures are told apart by these.
            for (std::size_t i = 0; i < shadow_zone_triangles_.size(); ++i) {
                if (shadow_zone_triangles_[i] != 0u) std::fprintf(stderr, "[shadow]   also %s: %u of %d textures\n", shadow_zone_names_[i].c_str(), shadow_zone_triangles_[i], shadow_zone_totals_[i]);
            }
        }
    }
    if (shadow_zone_ >= 0) {
        const auto &caves = live_->shadow_cave_zones;
        if (std::find(caves.begin(), caves.end(), shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)]) != caves.end()) cave_textures = true;
    }
    // The sliders follow: nothing but characters and monsters in a cave, everything out of one.
    const bool treat_as_cave = (shadow_in_cave_ || cave_textures) && !live_->shadow_whole_map_in_caves.load(std::memory_order_relaxed);
    if (const bool in_cave = treat_as_cave; in_cave != shadow_sliders_in_cave_) {
        shadow_sliders_in_cave_ = in_cave;
        live_->shadow_max_object = in_cave ? 30.0f : 39450.0f;
        live_->shadow_max_piece = in_cave ? 0.0f : 39450.0f;
    }
    if (treat_as_cave) whole_map = false;
    shadow_ranges_.clear();
    const auto triangle_casts = [&](const float *t, bool listed) {
        float e[3][3], longest = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float *p = t + 5 * i, *q = t + 5 * ((i + 1) % 3);
            for (int k = 0; k < 3; ++k) e[i][k] = q[k] - p[k];
            longest = std::max(longest, e[i][0] * e[i][0] + e[i][1] * e[i][1] + e[i][2] * e[i][2]);
        }
        if (std::sqrt(longest) > largest) return false;
        float n[3];
        cross(e[0], e[1], n);
        // Lying flat is an overlay on the ground, unless the list says otherwise (leaves that float, like lily pads).
        return listed || !normalize(n) || std::fabs(n[0] * up_axis[0] + n[1] * up_axis[1] + n[2] * up_axis[2]) < 0.95f;
    };
    // Zones 4, 5 and 6 of the swamp: their lily pads lie flat, and their texture does not stay the same from one game to the next.
    bool lily_pads_float = false;
    if (shadow_zone_ >= 0) {
        const std::string &zone_name = shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)];
        lily_pads_float = zone_name == "areas/hunting/swamp/area4" || zone_name == "areas/hunting/swamp/area5" || zone_name == "areas/hunting/swamp/area6";
    }
    // The jungle's camp: its waterfall is see-through water fading to nothing at its edges, and it cast as if it were rock.
    const bool water_fades = kJungleCampWater && shadow_zone_ >= 0 && shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/jungle/base";
    for (std::uint32_t i = 0; i < shadow_draws_.size(); ++i) {
        const ShadowDraw &d = shadow_draws_[i];
        if (d.count == 0 || d.target != rt.address) continue;
        // However small: nothing in the air casts. Cut-outs count as air by how clear they are, as leaves do, and those must.
        if (air(d) && !shadow_groups_[d.group].skinned && (!d.cutout || d.listed == 1)) continue;
        if (water_fades && d.blended && d.alpha_low < 16.0f && !shadow_groups_[d.group].skinned) continue;
        if (!whole_map) {
            // Characters and what the hunter carries, and nothing else: no plants, no objects.
            const ShadowGroup &g = shadow_groups_[d.group];
            bool carried = false;
            if (hunter != nullptr && !g.skinned && !d.cutout && g.size() <= 400.0f) {
                float a[3], b[3];
                centre_of(*hunter, a);
                centre_of(g, b);
                const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
                carried = dx * dx + dy * dy + dz * dz < 350.0f * 350.0f;
            }
            if (g.skinned || carried) shadow_ranges_.push_back({i, d.first, d.count});
            continue;
        }
        if (!d.cutout) {
            // Blended draws that stand up are scenery fading into the distance; flat ones are layers over the ground.
            const ShadowGroup &g = shadow_groups_[d.group];
            if (g.skinned || casts(g)) {
                shadow_ranges_.push_back({i, d.first, d.count});
            } else if (!backdrop(g) && !air(d) && !shell(d) && near_enough(d)) {
                const std::uint32_t start = static_cast<std::uint32_t>(shadow_vertices_.size() / 5u);
                for (std::uint32_t v = 0; v + 3u <= d.count; v += 3u) {
                    const std::size_t at = (static_cast<std::size_t>(d.first) + v) * 5u;
                    const float *t = shadow_vertices_.data() + at;
                    // Flat pieces and strips of ground, which overlap, would cast along their seams.
                    // Rock the game's list names is never ground, however low and flat. And what stands up, as a trunk's sides do, is
                    // no seam: those run along the ground.
                    if (d.listed != 2 && low_ground(t) && strip_or_flat(t) != 0 && !(d.size() <= largest_piece && d.height() >= 1.5f * d.width())) continue;
                    shadow_vertices_.resize(shadow_vertices_.size() + 15u);
                    std::copy_n(shadow_vertices_.begin() + static_cast<std::ptrdiff_t>(at), 15, shadow_vertices_.end() - 15);
                }
                const std::uint32_t added = static_cast<std::uint32_t>(shadow_vertices_.size() / 5u) - start;
                if (added != 0) shadow_ranges_.push_back({i, start, added});
            }
        } else if (casts(shadow_groups_[d.group]) || (!d.cutout && d.size() <= largest_piece && d.height() >= 1.5f * d.width())) {
            shadow_ranges_.push_back({i, d.first, d.count});
        } else if (d.cutout) {
            // The triangles that pass go after the picture's own vertices.
            const std::uint32_t start = static_cast<std::uint32_t>(shadow_vertices_.size() / 5u);
            for (std::uint32_t v = 0; v + 3u <= d.count; v += 3u) {
                const std::size_t at = (static_cast<std::size_t>(d.first) + v) * 5u;
                if (!triangle_casts(shadow_vertices_.data() + at, d.listed == 2 || lily_pads_float)) continue;
                shadow_vertices_.resize(shadow_vertices_.size() + 15u);
                std::copy_n(shadow_vertices_.begin() + static_cast<std::ptrdiff_t>(at), 15, shadow_vertices_.end() - 15);
            }
            const std::uint32_t added = static_cast<std::uint32_t>(shadow_vertices_.size() / 5u) - start;
            if (added != 0) shadow_ranges_.push_back({i, start, added});
        }
    }

    // The surfaces turned away from the sun, from the triangles the camera sees: each normal is turned towards the camera, and
    // points the triangles share average theirs, so that the shade follows the shape instead of showing every face.
    const auto cpu_sorted = std::chrono::steady_clock::now();
    // Working this out is slow and what it finds hardly changes: for a few flips the same triangles just move with the camera.
    const auto four_by_four = [](const std::array<float, 12> &m) {
        return interpolation::Matrix{m[0], m[1], m[2], 0.0f, m[3], m[4], m[5], 0.0f, m[6], m[7], m[8], 0.0f, m[9], m[10], m[11], 1.0f};
    };
    const interpolation::Matrix stage_eye = stage != nullptr ? interpolation::multiply(four_by_four(stage->view), four_by_four(stage->world)) : interpolation::Matrix{};
    float facing_motion[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    bool facing_reused = false;
    interpolation::Matrix facing_inverse{};
    if (whole_map && stage != nullptr && shadow_facing_reusable_ && shadow_facing_age_ < 3 && interpolation::affine_inverse(shadow_facing_eye_, facing_inverse)) {
        const interpolation::Matrix m = interpolation::multiply(stage_eye, facing_inverse);
        const float moved = std::sqrt(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]);
        float turned = 0.0f;
        for (std::size_t k = 0; k < 3; ++k) turned += (m[k] * shadow_facing_dir_[0] + m[4 + k] * shadow_facing_dir_[1] + m[8 + k] * shadow_facing_dir_[2]) * dir[k];
        if ((moved < 4000.0f && turned > 0.9999f) || shadow_left_known_ != 0u) {
            facing_reused = true;
            for (std::size_t c = 0; c < 4; ++c) {
                for (std::size_t k = 0; k < 3; ++k) facing_motion[3 * c + k] = m[4 * c + k];
            }
        }
    }
    if (facing_reused) ++shadow_facing_age_;
    const int next_map = facing_reused ? shadow_facing_held_ : 1 - shadow_facing_held_;
    if (!facing_reused) shadow_facing_.clear();
    if (whole_map && !facing_reused) {
        shadow_normals_.clear();
        shadow_points_.clear();
        std::vector<std::uint32_t> &points = shadow_sources_;
        points.resize(shadow_vertices_.size() / 5u);
        // Points are told apart by where they are in the world, not by their memory: the scenery is cut into pieces that do not
        // share vertices, and each piece would show its own edge otherwise.
        const auto point_of = [&](std::uint32_t vertex) {
            const float *p = shadow_vertices_.data() + static_cast<std::size_t>(vertex) * 5u;
            const float d[3] = {p[0] - world_origin[0], p[1] - world_origin[1], p[2] - world_origin[2]};
            const auto cell = [&](const float *axis) {
                const float along = d[0] * axis[0] + d[1] * axis[1] + d[2] * axis[2];
                return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(along / 6.0f + 0.5f)) & 0x1FFFFF);
            };
            const std::uint64_t key = (cell(right) << 42u) | (cell(up) << 21u) | cell(dir);
            const auto [it, added] = shadow_points_.try_emplace(key, static_cast<std::uint32_t>(shadow_normals_.size() / 3u));
            if (added) shadow_normals_.insert(shadow_normals_.end(), 3u, 0.0f);
            return it->second;
        };
        // Rock the game's list names is never a buried wall, and neither is a piece that rises well over the ground: a face of rock is
        // cut into triangles, and its lower one looks like a buried wall while its upper one shows what it is part of.
        // Thin strips along the seams of the ground. Up on a rock the same shape is a long triangle of its face, and must take the shade.
        const auto seam = [&](const ShadowDraw &d, const float *t) { return d.listed != 2 && low_ground(t) && strip_or_flat(t) == 1; };
        const auto may_be_buried = [&](const ShadowDraw &d) {
            if (d.listed == 2) return false;
            for (std::uint32_t v = d.first; v < d.first + d.count; ++v) {
                if (height_of(shadow_vertices_.data() + static_cast<std::size_t>(v) * 5u) >= feet + 400.0f) return false;
            }
            return true;
        };
        const auto solid = [&](const ShadowDraw &d) {
            const ShadowGroup &g = shadow_groups_[d.group];
            // A shell on the list takes the shade too (it shows through gaps in a wall), but it still casts nothing.
            return d.count != 0 && d.target == rt.address && !d.cutout && !air(d) && !g.skinned && !backdrop(g) && (!shell(d) || d.listed == 2);
        };
        for (const ShadowDraw &d : shadow_draws_) {
            if (!solid(d)) continue;
            for (std::uint32_t v = d.first; v + 3u <= d.first + d.count; v += 3u) {
                const float *a = shadow_vertices_.data() + static_cast<std::size_t>(v) * 5u, *b = a + 5, *c = a + 10;
                const float e0[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e1[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                float n[3];
                cross(e0, e1, n);
                if (seam(d, a) || (may_be_buried(d) && buried_wall(a))) { // neither must bend the ground around them
                    for (std::uint32_t k = 0; k < 3u; ++k) points[v + k] = point_of(v + k) * 3u;
                    continue;
                }
                if (n[0] * a[0] + n[1] * a[1] + n[2] * a[2] > 0.0f) {
                    for (float &x : n) x = -x;
                }
                for (std::uint32_t k = 0; k < 3u; ++k) {
                    const std::uint32_t at = point_of(v + k) * 3u;
                    points[v + k] = at;
                    for (std::uint32_t j = 0; j < 3u; ++j) shadow_normals_[at + j] += n[j];
                }
            }
        }
        const auto reach = [&](std::uint32_t at) {
            float n[3] = {shadow_normals_[at], shadow_normals_[at + 1u], shadow_normals_[at + 2u]};
            if (!normalize(n)) return 1.0f;
            const float x = std::clamp((n[0] * dir[0] + n[1] * dir[1] + n[2] * dir[2] + 0.05f) / 0.25f, 0.0f, 1.0f);
            return x * x * (3.0f - 2.0f * x);
        };
        const bool steep_only = live_->shadow_steep_only.load(std::memory_order_relaxed);
        for (const ShadowDraw &d : shadow_draws_) {
            if (!solid(d)) continue;
            for (std::uint32_t v = d.first; v + 3u <= d.first + d.count; v += 3u) {
                float r[3] = {reach(points[v]), reach(points[v + 1u]), reach(points[v + 2u])};
                // Thin upright strips between pieces of the ground are seams, not sides of anything.
                const float *t = shadow_vertices_.data() + static_cast<std::size_t>(v) * 5u;
                if (seam(d, t) || (may_be_buried(d) && buried_wall(t))) continue;
                // Normals are shared at the corners, which keeps low polygons from showing, but where a face of rock meets the ground its
                // corners lean up with the ground and its lower triangles would take the sun. What clearly turns away from it stays dark.
                // Only steep faces that stand tall: the ground's slopes and its small steps (the jungle's camp showed them as dark lines)
                // keep the light of their corners.
                const float rise = std::max({height_of(t), height_of(t + 5), height_of(t + 10)}) -
                                   std::min({height_of(t), height_of(t + 5), height_of(t + 10)});
                if (!steep_only || rise >= 250.0f) {
                    const float e0[3] = {t[5] - t[0], t[6] - t[1], t[7] - t[2]}, e1[3] = {t[10] - t[0], t[11] - t[1], t[12] - t[2]};
                    float n[3];
                    cross(e0, e1, n);
                    if (normalize(n) && (!steep_only || std::fabs(n[0] * up_axis[0] + n[1] * up_axis[1] + n[2] * up_axis[2]) < 0.7f)) {
                        if (n[0] * t[0] + n[1] * t[1] + n[2] * t[2] > 0.0f) {
                            for (float &x : n) x = -x;
                        }
                        const float x = std::clamp((n[0] * dir[0] + n[1] * dir[1] + n[2] * dir[2] + 0.25f) / 0.2f, 0.0f, 1.0f);
                        const float own = x * x * (3.0f - 2.0f * x);
                        for (float &v : r) v = std::min(v, own);
                    }
                }
                // Far away the coarse mountains would show their polygons: the shade fades out, and the game's own light stays.
                for (int k = 0; k < 3; ++k) {
                    const float *p = t + 5 * k;
                    const float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
                    const float x = std::clamp((std::sqrt(dx * dx + dy * dy + dz * dz) - 14000.0f) / 6000.0f, 0.0f, 1.0f);
                    r[k] += (1.0f - r[k]) * x * x * (3.0f - 2.0f * x);
                }
                if (r[0] >= 1.0f && r[1] >= 1.0f && r[2] >= 1.0f) continue; // all in the sun
                for (std::uint32_t k = 0; k < 3u; ++k) {
                    const float *p = shadow_vertices_.data() + static_cast<std::size_t>(v + k) * 5u;
                    const float real = shadow_depths_[v + k], w = shadow_proj_[3] * p[2] + shadow_proj_[5];
                    const float worked_out = ((shadow_proj_[2] * p[2] + shadow_proj_[4]) / w * shadow_vz_[0] + shadow_vz_[1]) / 65535.0f;
                    shadow_facing_.insert(shadow_facing_.end(), {p[0], p[1], p[2], r[k], real >= 0.0f ? real - worked_out : 0.0f});
                }
            }
        }
    }
    if (!facing_reused) {
    shadow_facing_count_[next_map] = static_cast<GLsizei>(shadow_facing_.size() / 5u);
    for (const ShadowDraw &d : shadow_draws_) {
        if (d.count == 0 || d.target != rt.address) continue;
        // What the list says is air, and whatever is taken for air or for a shell of the sky.
        // A veil that never gets near opaque, so what shows there is what lies behind it: the mouths of caves, foam on the sand.
        // That keeps its shade. The shapes in the sky do get near it, at the middle of their fades.
        // Not mist hanging far below the ground, as under the peak of zone 8 of the volcano: nothing lies behind it to keep any shade.
        bool far_below = shadow_zone_ >= 0 && shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/volcano/area8";
        for (std::uint32_t v = d.first; far_below && v < d.first + d.count; ++v) {
            far_below = height_of(shadow_vertices_.data() + static_cast<std::size_t>(v) * 5u) < feet - 1000.0f;
        }
        const bool veil = d.listed != 1 && d.alpha_high < 200.0f && !far_below;
        // What is taken for air counts high up, away from the ground it could lie over. The jungle's cave hangs its beams of light
        // right down to the floor, so there it counts at any height.
        bool taken_for_air = air(d) && !d.cutout;
        for (std::uint32_t v = d.first; taken_for_air && !air_reaches_ground && !far_below && v < d.first + d.count; ++v) {
            taken_for_air = height_of(shadow_vertices_.data() + static_cast<std::size_t>(v) * 5u) > feet + 600.0f;
        }
        // The tower's camp: its backdrop stands against the sky.
        const bool tower_backdrop = backdrop(shadow_groups_[d.group]) && shadow_zone_ >= 0 && shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/tower_1_2/base";
        const bool taken_for_sky = (shell(d) || tower_backdrop) && d.listed != 2;
        if (veil || (d.listed != 1 && !taken_for_air && !taken_for_sky)) continue;
        for (std::uint32_t v = d.first; v < d.first + d.count; ++v) {
            const float *q = shadow_vertices_.data() + static_cast<std::size_t>(v) * 5u;
            const float real = shadow_depths_[v], w = shadow_proj_[3] * q[2] + shadow_proj_[5];
            const float worked_out = ((shadow_proj_[2] * q[2] + shadow_proj_[4]) / w * shadow_vz_[0] + shadow_vz_[1]) / 65535.0f;
            shadow_facing_.insert(shadow_facing_.end(), {q[0], q[1], q[2], 1.0f, real >= 0.0f ? real - worked_out : 0.0f});
        }
    }
    shadow_clear_count_[next_map] = static_cast<GLsizei>(shadow_facing_.size() / 5u) - shadow_facing_count_[next_map];
    gl_.BindBuffer(GL_ARRAY_BUFFER, shadow_facing_vbo_[next_map]);
    gl_.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(shadow_facing_.size() * sizeof(float)), shadow_facing_.data(), GL_STREAM_DRAW);
    shadow_facing_held_ = next_map;
    shadow_facing_age_ = 0;
    shadow_facing_reusable_ = whole_map && stage != nullptr;
    shadow_facing_eye_ = stage_eye;
    std::copy(dir, dir + 3, shadow_facing_dir_);
    }
    const auto cpu_faced = std::chrono::steady_clock::now();

    static int runs = 0;
    static const int log_every = [] {
        const char *e = std::getenv("MHF_SHADOW_LOG");
        return e != nullptr ? std::max(1, std::atoi(e)) : 600;
    }();
    if (runs < 3 || runs % log_every == 0) {
        std::size_t solid_triangles = 0, blended_triangles = 0, cutout_triangles = 0, translucent_triangles = 0;
        for (const ShadowDraw &d : shadow_draws_) {
            if (d.target != rt.address) continue;
            (d.cutout ? cutout_triangles : d.blended ? blended_triangles : solid_triangles) += d.count / 3u;
            if (air(d)) translucent_triangles += d.count / 3u;
        }
        std::fprintf(stderr, "[shadow] triangles: %zu solid, %zu blended (%zu of them air), %zu cut-out; %u draws without depth left out; %zu shaded for facing away; map centred on %s\n",
                     solid_triangles, blended_triangles, translucent_triangles, cutout_triangles, shadow_skipped_, static_cast<std::size_t>(shadow_facing_count_[next_map]) / 3u, hunter != nullptr ? "the hunter" : "the camera");
    }
    if (runs++ % log_every == 0) {
        std::vector<const ShadowGroup *> by_size;
        for (const ShadowGroup &g : shadow_groups_) {
            if (g.triangles != 0) by_size.push_back(&g);
        }
        std::sort(by_size.begin(), by_size.end(), [](const ShadowGroup *x, const ShadowGroup *y) { return x->size() > y->size(); });
        std::fprintf(stderr, "[shadow] %zu objects, %zu vertices, %zu ranges cast, largest allowed %g, game light %d, sun height %g degrees\n",
                     by_size.size(), shadow_vertices_.size() / 5u, shadow_ranges_.size(), largest, shadow_light_ok_ ? 1 : 0,
                     std::asin(std::clamp(dir[0] * up_axis[0] + dir[1] * up_axis[1] + dir[2] * up_axis[2], -1.0f, 1.0f)) * 57.29578f);
        for (std::size_t i = 0; i < by_size.size() && i < 12u; ++i) {
            std::fprintf(stderr, "[shadow]   size %.0f, %u triangles, %s, %s\n", by_size[i]->size(), by_size[i]->triangles,
                         by_size[i]->skinned ? "skinned" : "rigid", casts(*by_size[i]) ? "casts" : "left out");
        }
    }

    shadow_cur_ = 1 - shadow_cur_;
    ShadowPass &p = shadow_pass_;
    p.valid = true;
    p.generation = ++shadow_generation_;
    p.address = rt.address;
    p.map = shadow_cur_;
    p.facing = shadow_facing_held_;
    std::copy(facing_motion, facing_motion + 12, p.facing_motion);
    std::copy(shadow_proj_, shadow_proj_ + 6, p.proj);
    std::copy(shadow_vz_, shadow_vz_ + 2, p.vz);
    std::copy(shadow_vp_, shadow_vp_ + 4, p.vp);
    std::copy(center, center + 3, p.center);
    std::copy(right, right + 3, p.right);
    std::copy(up, up + 3, p.up);
    std::copy(dir, dir + 3, p.dir);
    p.half = shadow_reach_;
    p.drop = whole_map ? 1e9f : shadow_drop_; // with the scenery in the map, nothing has to be cut off below a caster
    p.darkness = std::clamp(live_->shadow_strength.load(std::memory_order_relaxed), 0.0f, 1.0f);
    // The zone's, unless the slider was moved from where it starts.
    if (zone_darkness >= 0.0f && std::fabs(p.darkness - 0.4f) < 0.001f) p.darkness = zone_darkness;
    // Zone 5 of the desert by day: in a sandstorm the game dims the sun, and the shadows stood out black. Down to 10% once
    // the sun is well dimmed (it goes from about e0 to 40 as the storm comes), and back as it clears; over a second or two, not
    // at once.
    if (!shadow_night_ && shadow_zone_ >= 0 && shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/desert/area5") {
        const std::uint32_t sun = shadow_light_colours_[2];
        const float most = static_cast<float>(std::max({sun & 0xFFu, sun >> 8u & 0xFFu, sun >> 16u & 0xFFu}));
        const float clear = std::clamp((most - 72.0f) / 40.0f, 0.0f, 1.0f); // 0 at 0x48 and under, 1 from 0x70
        const float wanted = std::min(p.darkness, 0.10f + (p.darkness - 0.10f) * clear);
        const auto now = std::chrono::steady_clock::now();
        if (shadow_storm_darkness_ < 0.0f) {
            shadow_storm_darkness_ = wanted;
        } else {
            const float seconds = std::chrono::duration<float>(now - shadow_storm_time_).count();
            shadow_storm_darkness_ += (wanted - shadow_storm_darkness_) * std::clamp(seconds / 1.5f, 0.0f, 1.0f);
        }
        shadow_storm_time_ = now;
        p.darkness = shadow_storm_darkness_;
    } else {
        shadow_storm_darkness_ = -1.0f;
    }
    shadow_darkness_shown_ = p.darkness;
    // Zones 1, 2, 4, 5 and 6 of the swamp, 8 and 10 of the forest and hills, 1, 2, 3 and 7 of the old jungle, and the old swamp's camp and zones 1, 2, 4 and 9 hang in mist: far shadows fade with the game's fog.
    const std::string &fog_zone = shadow_zone_ >= 0 ? shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] : std::string();
    const bool fog_fades = fog_zone == "areas/hunting/swamp/area1" || fog_zone == "areas/hunting/swamp/area2" || fog_zone == "areas/hunting/swamp/area4" ||
                           fog_zone == "areas/hunting/swamp/area5" || fog_zone == "areas/hunting/swamp/area6" ||
                           fog_zone == "areas/hunting/forestandhills/area8" || fog_zone == "areas/hunting/forestandhills/area10" ||
                           fog_zone == "areas/hunting/old_jungle/area1" || fog_zone == "areas/hunting/old_jungle/area2" ||
                           fog_zone == "areas/hunting/old_jungle/area3" || fog_zone == "areas/hunting/old_jungle/area7" ||
                           fog_zone == "areas/hunting/old_swamp/base" || fog_zone == "areas/hunting/old_swamp/area1" ||
                           fog_zone == "areas/hunting/old_swamp/area2" || fog_zone == "areas/hunting/old_swamp/area4" ||
                           fog_zone == "areas/hunting/old_swamp/area9";
    p.fog[0] = shadow_fog_[0];
    p.fog[1] = shadow_fog_[1];
    // The forest's mist is lighter: a fifth of the swamp's fade (10 on the slider where the swamp has 50).
    const bool light_mist = fog_zone == "areas/hunting/forestandhills/area8" || fog_zone == "areas/hunting/forestandhills/area10" ||
                            fog_zone.rfind("areas/hunting/old_swamp/", 0) == 0; // and the old swamp's, the same
    // The old jungle's, half of it (25 where the swamp has 50).
    const bool half_mist = fog_zone.rfind("areas/hunting/old_jungle/", 0) == 0;
    p.fog[2] = fog_fades && shadow_fog_frame_ == frame_ ? live_->shadow_mist_fade.load(std::memory_order_relaxed) * (light_mist ? 0.2f : half_mist ? 0.5f : 1.0f) : 0.0f;
    // The player and what it carries, on a small map of its own.
    shadow_player_groups_.clear();
    p.player_half = 0.0f;
    if (hunter != nullptr) {
        p.player_half = 800.0f;
        float c[3];
        centre_of(*hunter, c);
        const float player_texel = 2.0f * p.player_half / static_cast<float>(kShadowPlayerSize);
        float across[2] = {}, world_across[2] = {}, along = 0.0f;
        for (int k = 0; k < 3; ++k) {
            across[0] += c[k] * right[k];
            across[1] += c[k] * up[k];
            along += c[k] * dir[k];
            world_across[0] += world_origin[k] * right[k];
            world_across[1] += world_origin[k] * up[k];
        }
        for (int i = 0; i < 2; ++i) across[i] = std::floor((across[i] - world_across[i]) / player_texel) * player_texel + world_across[i];
        for (int k = 0; k < 3; ++k) p.player_center[k] = right[k] * across[0] + up[k] * across[1] + dir[k] * along;
        for (std::uint32_t i = 0; i < shadow_groups_.size(); ++i) {
            const ShadowGroup &g = shadow_groups_[i];
            if (g.triangles == 0) continue;
            float o[3];
            centre_of(g, o);
            const float dx = o[0] - c[0], dy = o[1] - c[1], dz = o[2] - c[2];
            if (&g == hunter || (!g.skinned && g.size() <= 400.0f && dx * dx + dy * dy + dz * dz < 350.0f * 350.0f)) shadow_player_groups_.push_back(i);
        }
    }
    // What was mapped stays, for the in-between pictures to map it again with the characters where they are by then.
    ShadowKept &kept = shadow_kept_[p.generation % 3u];
    kept.generation = p.generation;
    kept.vertices = shadow_vertices_;
    kept.ranges.clear();
    for (const ShadowRange &r : shadow_ranges_) {
        const ShadowDraw &d = shadow_draws_[r.draw];
        KeptRange k;
        k.first = r.first;
        k.count = r.count;
        k.vertices = d.vertices;
        k.texture = d.texture != 0 && d.func != 1 ? d.texture : 0;
        std::copy(d.xform, d.xform + 4, k.xform);
        k.func = d.texture != 0 ? d.func : 1;
        k.ref = d.ref;
        k.mask = d.mask;
        k.clamp_s = d.clamp_s;
        k.clamp_t = d.clamp_t;
        // Trunks and posts are scenery to the game, but as thin as the soft edge of the scenery's shadows: theirs would vanish.
        k.scenery = !(d.cutout || casts(shadow_groups_[d.group]) || d.height() >= 1.5f * d.width());
        k.player = std::find(shadow_player_groups_.begin(), shadow_player_groups_.end(), d.group) != shadow_player_groups_.end();
        // Not the cut-outs: their shape comes from a texture, and the one remembered may be gone by the time the map is drawn again.
        k.fixed = whole_map && !shadow_groups_[d.group].skinned && !casts(shadow_groups_[d.group]) && !d.cutout;
        kept.ranges.push_back(k);
    }
    // The scenery does not move, so its shadows need not be drawn again: what has been seen of it is kept (and goes on casting
    // when the camera turns away), its map is copied, and only what moves is drawn over that.
    bool scenery_kept = false;
    // Zone 6 of the desert loses all its shadows with this, coming from zone 7, for a reason not found yet; zone 9 of the jungle
    // gets them wrong coming from zone 7 the same way.
    // Zone 2 of the snowy mountains has two floors, and what was kept from one is wrong on the other.
    const bool nothing_kept_here = shadow_zone_ >= 0 && (shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/desert/area6" ||
                                                         shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/jungle/area9" ||
                                                         shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] == "areas/hunting/snowy_mountains/area2");
    if (nothing_kept_here && shadow_known_zone_ != shadow_zone_) {
        shadow_forget_scenery();
        shadow_known_zone_ = shadow_zone_;
    }
    if (whole_map && stage != nullptr && !nothing_kept_here && live_->shadow_keep_scenery.load(std::memory_order_relaxed)) {
        // What was kept belongs to the zone it was seen in, and its sun: nothing of it carries over to another.
        if (shadow_known_zone_ != shadow_zone_) shadow_forget_scenery();
        if (shadow_seen_fbo_ == 0) {
            gl_.ActiveTexture(GL_TEXTURE0);
            gl_.GenTextures(1, &shadow_seen_color_);
            gl_.BindTexture(GL_TEXTURE_2D, shadow_seen_color_);
            gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_R8, shadow_size_, shadow_size_, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
            set_texture_params(false, true, true);
            gl_.GenTextures(1, &shadow_seen_depth_);
            gl_.BindTexture(GL_TEXTURE_2D, shadow_seen_depth_);
            gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, shadow_size_, shadow_size_, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
            set_texture_params(false, true, true);
            gl_.GenFramebuffers(1, &shadow_seen_fbo_);
            gl_.BindFramebuffer(GL_FRAMEBUFFER, shadow_seen_fbo_);
            gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shadow_seen_color_, 0);
            gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow_seen_depth_, 0);
        }
        // The frame: the biggest group holding a piece that gave it before. None: another area, and everything starts again.
        const ShadowGroup *anchor = nullptr;
        for (const ShadowDraw &d : shadow_draws_) {
            const ShadowGroup &g = shadow_groups_[d.group];
            if (d.count == 0 || g.skinned || shadow_seen_anchor_.count(d.vertices) == 0) continue;
            if (anchor == nullptr || g.triangles > anchor->triangles) anchor = &g;
        }
        if (anchor == nullptr) {
            shadow_forget_scenery();
            anchor = stage;
        }
        for (const ShadowDraw &d : shadow_draws_) {
            if (&shadow_groups_[d.group] == anchor) shadow_seen_anchor_.insert(d.vertices);
        }
        const interpolation::Matrix to_eye = interpolation::multiply(four_by_four(anchor->view), four_by_four(anchor->world));
        interpolation::Matrix to_seen{};
        if (interpolation::affine_inverse(to_eye, to_seen)) {
            const auto point = [](const interpolation::Matrix &m, const float *x, float *out) {
                for (std::size_t k = 0; k < 3; ++k) out[k] = m[k] * x[0] + m[4 + k] * x[1] + m[8 + k] * x[2] + m[12 + k];
            };
            const auto way = [&](const interpolation::Matrix &m, const float *x, float *out) {
                for (std::size_t k = 0; k < 3; ++k) out[k] = m[k] * x[0] + m[4 + k] * x[1] + m[8 + k] * x[2];
                normalize(out);
            };
            // A piece is known by its vertices, its texture and where it stands. Most of what is on the screen should be known by
            // now; if not, the frame is not the one it was, and nothing is learnt from this picture.
            std::vector<std::size_t> fresh;
            std::size_t seen = shadow_left_known_, known = shadow_left_known_;
            std::vector<std::uint64_t> fresh_keys;
            for (std::size_t i = 0; i < kept.ranges.size(); ++i) {
                const KeptRange &r = kept.ranges[i];
                if (!r.fixed) continue;
                ++seen;
                float at[3];
                point(to_seen, kept.vertices.data() + static_cast<std::size_t>(r.first) * 5u, at);
                std::uint64_t key = 1469598103934665603ull;
                const auto mix = [&key](std::uint64_t v) { key = (key ^ v) * 1099511628211ull; };
                mix(r.vertices);
                mix(r.count);
                mix(shadow_draws_[shadow_ranges_[i].draw].texture_hash);
                for (const float c : at) mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(c / 32.0f))));
                if (shadow_seen_keys_.count(key) != 0) {
                    ++known;
                } else {
                    fresh.push_back(i);
                    fresh_keys.push_back(key);
                }
            }
            const bool trusted = shadow_seen_keys_.size() < 64u || known * 4u >= seen;
            // What needs no looking at from now on: the scenery that is kept, and that which casts nothing.
            if (shadow_known_zone_ != shadow_zone_ || !trusted) shadow_known_.clear();
            shadow_known_zone_ = shadow_zone_;
            if (trusted) {
                std::unordered_set<std::uint32_t> not_kept_yet;
                for (const std::size_t i : fresh) not_kept_yet.insert(shadow_ranges_[i].draw);
                for (std::uint32_t i = 0; i < shadow_draws_.size(); ++i) {
                    const ShadowDraw &d = shadow_draws_[i];
                    const ShadowGroup &g = shadow_groups_[d.group];
                    if (d.known_key == 0u || d.count == 0 || g.skinned || casts(g) || d.cutout || d.target != rt.address || air(d) || shell(d) || backdrop(g) || not_kept_yet.count(i) != 0) continue;
                    KnownPiece known_piece;
                    std::copy(d.low, d.low + 3, known_piece.low);
                    std::copy(d.high, d.high + 3, known_piece.high);
                    known_piece.triangles = d.count / 3u;
                    shadow_known_[d.known_key] = known_piece;
                }
            }
            shadow_seen_doubts_ = trusted ? 0 : shadow_seen_doubts_ + 1;
            if (shadow_seen_doubts_ >= 30) shadow_forget_scenery();
            if (trusted && shadow_seen_vertices_.size() < 6000000u) {
                for (std::size_t n = 0; n < fresh.size(); ++n) {
                    KeptRange r = kept.ranges[fresh[n]];
                    const std::uint32_t first = static_cast<std::uint32_t>(shadow_seen_vertices_.size() / 5u);
                    shadow_seen_vertices_.resize(shadow_seen_vertices_.size() + static_cast<std::size_t>(r.count) * 5u);
                    float *out = shadow_seen_vertices_.data() + static_cast<std::size_t>(first) * 5u;
                    const float *in = kept.vertices.data() + static_cast<std::size_t>(r.first) * 5u;
                    for (std::uint32_t v = 0; v < r.count; ++v, in += 5, out += 5) {
                        point(to_seen, in, out);
                        out[3] = in[3];
                        out[4] = in[4];
                    }
                    r.first = first;
                    // Solid as it is kept: plain surfaces too have a low alpha test on, and by the time the map is drawn again their
                    // texture may be gone, or be another one by then, and the test would throw away the rock.
                    r.texture = 0;
                    r.func = 1;
                    shadow_seen_ranges_.push_back(r);
                    shadow_seen_keys_.insert(fresh_keys[n]);
                    shadow_seen_grew_ = true;
                }
            }
            if (trusted && !shadow_seen_ranges_.empty()) {
                float seen_dir[3] = {};
                if (shadow_seen_built_) way(to_eye, shadow_seen_dir_, seen_dir);
                float now[3] = {};
                if (shadow_seen_built_) point(to_seen, center, now);
                const float dx = now[0] - shadow_seen_center_[0], dy = now[1] - shadow_seen_center_[1], dz = now[2] - shadow_seen_center_[2];
                const bool moved = dx * dx + dy * dy + dz * dz > 0.09f * shadow_reach_ * shadow_reach_;
                const bool turned = seen_dir[0] * dir[0] + seen_dir[1] * dir[1] + seen_dir[2] * dir[2] < 0.9999f;
                ++shadow_seen_age_;
                if (!shadow_seen_built_ || moved || turned || (shadow_seen_grew_ && shadow_seen_age_ >= 20)) {
                    point(to_seen, center, shadow_seen_center_);
                    way(to_seen, right, shadow_seen_right_);
                    way(to_seen, up, shadow_seen_up_);
                    way(to_seen, dir, shadow_seen_dir_);
                    shadow_seen_map_.vertices = shadow_seen_vertices_;
                    for (std::size_t v = 0; v + 5u <= shadow_seen_map_.vertices.size(); v += 5u) {
                        float *x = shadow_seen_map_.vertices.data() + v;
                        const float world[3] = {x[0], x[1], x[2]};
                        point(to_eye, world, x);
                    }
                    shadow_seen_map_.ranges = shadow_seen_ranges_;
                    ShadowPass whole = p;
                    draw_shadow_map(whole, shadow_seen_map_, shadow_seen_map_.vertices, false, shadow_seen_fbo_);
                    shadow_seen_built_ = true;
                    shadow_seen_grew_ = false;
                    shadow_seen_age_ = 0;
                    ++shadow_seen_builds_;
                }
                // The map hangs where it was drawn, seen from this picture.
                point(to_eye, shadow_seen_center_, p.center);
                way(to_eye, shadow_seen_right_, p.right);
                way(to_eye, shadow_seen_up_, p.up);
                way(to_eye, shadow_seen_dir_, p.dir);
                scenery_kept = true;
            }
        }
    }
    const auto cpu_kept = std::chrono::steady_clock::now();
    shadow_time_begin(0);
    if (scenery_kept) {
        gl_.Disable(GL_SCISSOR_TEST);
        gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl_.DepthMask(GL_TRUE);
        gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, shadow_seen_fbo_);
        gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, shadow_fbo_[p.map]);
        gl_.BlitFramebuffer(0, 0, shadow_size_, shadow_size_, 0, 0, shadow_size_, shadow_size_, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    }
    draw_shadow_map(p, kept, kept.vertices, false, 0, scenery_kept);
    shadow_time_end();
    if (p.player_half > 0.0f) {
        shadow_time_begin(1);
        draw_shadow_map(p, kept, kept.vertices, true);
        shadow_time_end();
    }
    static constexpr float same_eye[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    apply_shadow(rt, p, same_eye);
    const auto cpu_end = std::chrono::steady_clock::now();
    const auto ms = [](auto from, auto to) { return std::chrono::duration<double, std::milli>(to - from).count(); };
    shadow_cpu_ms_[0] += ms(cpu_start, cpu_sorted);
    shadow_cpu_ms_[1] += ms(cpu_sorted, cpu_faced);
    shadow_cpu_ms_[2] += ms(cpu_faced, cpu_kept);
    shadow_cpu_ms_[3] += ms(cpu_kept, cpu_end);
    if (++shadow_cpu_runs_ >= 150) {
        const double n = shadow_cpu_runs_;
        const auto gpu = [&](int i) {
            const double v = shadow_time_[i].count != 0 ? shadow_time_[i].ms / shadow_time_[i].count : 0.0;
            shadow_time_[i].ms = 0.0;
            shadow_time_[i].count = 0;
            return v;
        };
        const double g0 = gpu(0), g1 = gpu(1), g2 = gpu(2), g3 = gpu(3), g4 = gpu(4);
        std::fprintf(stderr, "[shadow left] %u pieces left alone in this picture, %zu known\n", shadow_left_known_, shadow_known_.size());
        std::fprintf(stderr, "[shadow time] cpu ms per frame: capture %.2f, sorting %.2f, facing %.2f, keeping %.2f, sending %.2f | card ms each: map %.2f, hunter's map %.2f, factor %.2f, facing %.2f, applying %.2f | ground pieces agreeing %.0f%% | scenery kept: %zu pieces, %zu vertices, map drawn %d times\n",
                     shadow_capture_ms_ / n, shadow_cpu_ms_[0] / n, shadow_cpu_ms_[1] / n, shadow_cpu_ms_[2] / n, shadow_cpu_ms_[3] / n, g0, g1, g2, g3, g4,
                     shadow_scene_pictures_ != 0 ? 100.0 * shadow_scene_share_ / shadow_scene_pictures_ : 0.0,
                     shadow_seen_ranges_.size(), shadow_seen_vertices_.size() / 5u, shadow_seen_builds_);
        shadow_seen_builds_ = 0;
        shadow_scene_share_ = 0.0;
        shadow_scene_pictures_ = 0;
        shadow_capture_ms_ = 0.0;
        for (double &v : shadow_cpu_ms_) v = 0.0;
        shadow_cpu_runs_ = 0;
    }

    // The view for telling what casts: skinned green, placed objects yellow, what was left out red.
    if (live_->shadow_show.load(std::memory_order_relaxed)) {
        const GLuint show = shadow_show_program_;
        gl_.BindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
        gl_.Viewport(0, 0, static_cast<GLsizei>(rt.width) * scale_, static_cast<GLsizei>(rt.height) * scale_);
        gl_.Disable(GL_DEPTH_TEST);
        gl_.DepthMask(GL_FALSE);
        gl_.Enable(GL_BLEND);
        gl_.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        gl_.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        gl_.UseProgram(show);
        gl_.Uniform4f(uniform(show, "u_proj"), p.proj[0], p.proj[1], p.proj[2], p.proj[3]);
        gl_.Uniform2f(uniform(show, "u_proj_z"), p.proj[4], p.proj[5]);
        gl_.Uniform4f(uniform(show, "u_vp"), p.vp[0], p.vp[1], p.vp[2], p.vp[3]);
        gl_.Uniform2f(uniform(show, "u_target"), static_cast<float>(rt.width), static_cast<float>(rt.height));
        const GLint l_color = uniform(show, "u_color");
        gl_.BindVertexArray(shadow_vao_);
        for (const ShadowDraw &d : shadow_draws_) {
            if (d.count == 0 || d.target != rt.address) continue;
            const ShadowGroup &g = shadow_groups_[d.group];
            // Why it is left out: taken for air, for a backdrop, for a shell around the camera, or anything else.
            if (air(d)) gl_.Uniform4f(l_color, 1.0f, 0.1f, 1.0f, 0.5f);
            else if (backdrop(g)) gl_.Uniform4f(l_color, 0.1f, 1.0f, 1.0f, 0.5f);
            else if (shell(d)) gl_.Uniform4f(l_color, 1.0f, 1.0f, 1.0f, 0.5f);
            else gl_.Uniform4f(l_color, 1.0f, 0.1f, 0.1f, 0.25f);
            gl_.DrawArrays(GL_TRIANGLES, static_cast<GLint>(d.first), static_cast<GLsizei>(d.count));
        }
        for (const ShadowRange &r : shadow_ranges_) {
            const ShadowDraw &d = shadow_draws_[r.draw];
            const ShadowGroup &g = shadow_groups_[d.group];
            if (g.skinned) gl_.Uniform4f(l_color, 0.1f, 1.0f, 0.2f, 0.7f);
            else if (!casts(g) && !d.cutout) gl_.Uniform4f(l_color, 0.1f, 0.4f, 1.0f, 0.7f); // a trunk found in the scenery
            else gl_.Uniform4f(l_color, 1.0f, 0.9f, 0.1f, 0.7f);
            gl_.DrawArrays(GL_TRIANGLES, static_cast<GLint>(r.first), static_cast<GLsizei>(r.count));
        }
        gl_.Disable(GL_BLEND);
    }

    // Identifying things: what is under the middle of the screen, nearest first, in the log, with the nearest painted white.
    if (live_->shadow_pick.load(std::memory_order_relaxed)) {
        struct Hit {
            float distance;
            std::uint32_t draw;
        };
        std::vector<Hit> hits;
        const float cx = static_cast<float>(rt.width) * 0.5f, cy = 136.0f;
        for (std::uint32_t i = 0; i < shadow_draws_.size(); ++i) {
            const ShadowDraw &d = shadow_draws_[i];
            if (d.count == 0 || d.target != rt.address) continue;
            float nearest = 1e30f;
            for (std::uint32_t v = 0; v + 3u <= d.count; v += 3u) {
                const float *t = shadow_vertices_.data() + (static_cast<std::size_t>(d.first) + v) * 5u;
                float x[3], y[3], z[3];
                bool front = true;
                for (int k = 0; k < 3; ++k) {
                    const float *q = t + 5 * k;
                    const float w = p.proj[3] * q[2] + p.proj[5];
                    if (w <= 0.0f) front = false;
                    else {
                        x[k] = q[0] * p.proj[0] / w * p.vp[0] + p.vp[2];
                        y[k] = q[1] * p.proj[1] / w * p.vp[1] + p.vp[3];
                        z[k] = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
                    }
                }
                if (!front) continue;
                const float a = (x[1] - x[0]) * (cy - y[0]) - (y[1] - y[0]) * (cx - x[0]);
                const float b = (x[2] - x[1]) * (cy - y[1]) - (y[2] - y[1]) * (cx - x[1]);
                const float c = (x[0] - x[2]) * (cy - y[2]) - (y[0] - y[2]) * (cx - x[2]);
                if ((a >= 0.0f && b >= 0.0f && c >= 0.0f) || (a <= 0.0f && b <= 0.0f && c <= 0.0f)) nearest = std::min({nearest, z[0], z[1], z[2]});
            }
            if (nearest < 1e29f) hits.push_back({nearest, i});
        }
        std::sort(hits.begin(), hits.end(), [](const Hit &x, const Hit &y) { return x.distance < y.distance; });
        std::uint64_t signature = hits.size();
        for (std::size_t i = 0; i < hits.size() && i < 6u; ++i) signature = (signature * 1099511628211ull ^ shadow_draws_[hits[i].draw].texture_hash) + shadow_draws_[hits[i].draw].count;
        if (signature != shadow_pick_logged_) {
            shadow_pick_logged_ = signature;
            std::fprintf(stderr, "[shadow] centre of the screen: %zu things\n", hits.size());
            // What the map left out, nearest first: the game's own round shadows are likely among it.
            struct LeftOut {
                std::uint32_t draws{};
                float nearest{1e30f}, alpha{}, red{};
            };
            std::map<std::array<std::uint64_t, 4>, LeftOut> left_out;
            for (const UndepthedDraw &u : shadow_undepthed_) {
                LeftOut &seen = left_out[{u.texture, static_cast<std::uint64_t>(u.paint[0]) << 32u | u.paint[3], static_cast<std::uint64_t>(u.paint[1]) << 32u | u.paint[2], static_cast<std::uint64_t>(u.paint[4]) << 32u | u.paint[5]}];
                ++seen.draws;
                seen.nearest = std::min(seen.nearest, std::sqrt(u.eye[0] * u.eye[0] + u.eye[1] * u.eye[1] + u.eye[2] * u.eye[2]));
                seen.alpha = u.alpha;
                seen.red = u.red;
            }
            std::fprintf(stderr, "[shadow] left out of the map: %zu draws, %zu kinds\n", shadow_undepthed_.size(), left_out.size());
            // The dark ones, gathered by where they are: how many round shadows there are.
            std::vector<std::array<float, 4>> rounds;
            for (const UndepthedDraw &u : shadow_undepthed_) {
                if ((u.paint[3] & 32u) == 0u || u.texture != 0x67c2a188780d8924ull) continue;
                bool found = false;
                for (auto &round : rounds) {
                    const float dx = round[0] - u.eye[0], dy = round[1] - u.eye[1], dz = round[2] - u.eye[2];
                    if (dx * dx + dy * dy + dz * dz < 150.0f * 150.0f) {
                        round[3] += 1.0f;
                        found = true;
                        break;
                    }
                }
                if (!found) rounds.push_back({u.eye[0], u.eye[1], u.eye[2], 1.0f});
            }
            std::fprintf(stderr, "[shadow] dark pieces gathered into %zu places:", rounds.size());
            for (const auto &round : rounds) std::fprintf(stderr, " %.0f pieces at %.0f;", round[3], std::sqrt(round[0] * round[0] + round[1] * round[1] + round[2] * round[2]));
            std::fprintf(stderr, "\n");
            for (const auto &[key, seen] : left_out) {
                std::fprintf(stderr, "[shadow]   texture %016llx blend %08llx flags %llu fixed %016llx texfunc+alpha %016llx: %u draws, nearest %.0f, colour %.0f alpha %.0f\n", static_cast<unsigned long long>(key[0]),
                             static_cast<unsigned long long>(key[1] >> 32u), static_cast<unsigned long long>(key[1] & 0xFFFFFFFFull), static_cast<unsigned long long>(key[2]), static_cast<unsigned long long>(key[3]), seen.draws, seen.nearest, seen.red, seen.alpha);
            }
            // Every way of painting in this picture, once for each set of them: what never paints shows up without aiming at it.
            struct PaintStyle {
                std::uint32_t draws{}, triangles{};
                float lowest{1e30f}, highest{-1e30f}, biggest{};
                std::unordered_set<std::uint64_t> textures;
            };
            std::map<std::array<std::uint32_t, 8>, PaintStyle> styles;
            for (const ShadowDraw &d : shadow_draws_) {
                if (d.count == 0 || d.target != rt.address || shadow_groups_[d.group].skinned) continue;
                PaintStyle &style = styles[{d.blended ? 1u : 0u, d.paint[0], d.paint[1], d.paint[2], d.paint[3], d.paint[4], d.paint[5], d.paint[6]}];
                ++style.draws;
                style.triangles += d.count / 3u;
                style.biggest = std::max(style.biggest, d.size());
                style.textures.insert(d.texture_hash);
                for (std::uint32_t v = d.first; v < d.first + d.count; ++v) {
                    const float height = height_of(shadow_vertices_.data() + static_cast<std::size_t>(v) * 5u) - feet;
                    style.lowest = std::min(style.lowest, height);
                    style.highest = std::max(style.highest, height);
                }
            }
            std::uint64_t styles_seen = 1469598103934665603ull;
            for (const auto &[key, style] : styles) {
                for (const std::uint32_t k : key) styles_seen = (styles_seen ^ k) * 1099511628211ull;
            }
            static std::uint64_t styles_logged = 0u;
            if (styles_seen != styles_logged) {
                styles_logged = styles_seen;
                std::fprintf(stderr, "[shadow] ways of painting in this picture: %zu\n", styles.size());
                for (const auto &[key, style] : styles) {
                    std::fprintf(stderr, "[shadow]   %s blend %06x %06x %06x mask %06x alpha test %08x texfunc %06x flags %u: %u draws, %u triangles, %zu textures, biggest %.0f, height %.0f to %.0f;", key[0] != 0u ? "blended" : "solid",
                                 key[1], key[2], key[3], key[4], key[5], key[6], key[7], style.draws, style.triangles, style.textures.size(), style.biggest, style.lowest, style.highest);
                    int named = 0;
                    for (const std::uint64_t texture : style.textures) {
                        if (named++ == 4) break;
                        std::fprintf(stderr, " %016llx", static_cast<unsigned long long>(texture));
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            // Which points two pieces share: scenery is joined to its neighbours, what floats in the air is not.
            std::unordered_map<std::uint64_t, std::uint32_t> points_of; // the draw that has it, plus 1; 0: more than one
            const auto point_key = [&](const float *x) {
                std::uint64_t key = 1469598103934665603ull;
                for (int k = 0; k < 3; ++k) key = (key ^ static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(x[k] / 2.0f + 0.5f)))) * 1099511628211ull;
                return key;
            };
            for (std::uint32_t n = 0; n < shadow_draws_.size(); ++n) {
                const ShadowDraw &o = shadow_draws_[n];
                if (o.count == 0 || o.target != rt.address || shadow_groups_[o.group].skinned) continue;
                for (std::uint32_t v = o.first; v < o.first + o.count; ++v) {
                    const auto [at, fresh] = points_of.try_emplace(point_key(shadow_vertices_.data() + static_cast<std::size_t>(v) * 5u), n + 1u);
                    if (!fresh && at->second != n + 1u) at->second = 0u;
                }
            }
            for (std::size_t i = 0; i < hits.size() && i < 6u; ++i) {
                const ShadowDraw &d = shadow_draws_[hits[i].draw];
                const ShadowGroup &g = shadow_groups_[d.group];
                bool in_map = false;
                for (const ShadowRange &r : shadow_ranges_) in_map = in_map || r.draw == hits[i].draw;
                std::uint32_t joined = 0u;
                float lowest = 1e30f, highest = -1e30f;
                for (std::uint32_t v = d.first; v < d.first + d.count; ++v) {
                    const float *x = shadow_vertices_.data() + static_cast<std::size_t>(v) * 5u;
                    const auto at = points_of.find(point_key(x));
                    joined += at != points_of.end() && at->second == 0u ? 1u : 0u;
                    lowest = std::min(lowest, height_of(x) - feet);
                    highest = std::max(highest, height_of(x) - feet);
                }
                std::fprintf(stderr, "[shadow]   #%zu joined to others %u of %u points, height %.0f to %.0f, fog leaves %.2f to %.2f of colour %06x, blend %06x %06x %06x mask %06x alpha test %08x texfunc %06x flags %u\n", i + 1u, joined, d.count, lowest, highest, d.fog_low, d.fog_high, d.fog_color,
                             d.paint[0], d.paint[1], d.paint[2], d.paint[3], d.paint[4], d.paint[5], d.paint[6]);
                std::fprintf(stderr, "[shadow]   #%zu texture %016llx distance %.0f, %u triangles, size %.0f, %s%s%s%s, clear %.0f%%, vertex alpha %.0f-%.0f, %s, %s\n", i + 1u,
                             static_cast<unsigned long long>(d.texture_hash), hits[i].distance, d.count / 3u, d.size(), g.skinned ? "skinned " : "", d.cutout ? "cut-out " : "",
                             d.blended ? "blended " : "solid ", air(d) ? "air" : backdrop(g) ? "backdrop" : shell(d) ? "shell" : "", d.holes * 100.0f, d.alpha_low, d.alpha_high,
                             in_map ? "casts" : "does not cast", !d.cutout && !air(d) && !g.skinned && !backdrop(g) && (!shell(d) || d.listed == 2) ? "can shade" : "never shades");
            }
        }
        const GLuint show = shadow_show_program_;
        gl_.BindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
        const GLsizei tw = static_cast<GLsizei>(rt.width) * scale_, th = static_cast<GLsizei>(rt.height) * scale_;
        gl_.Viewport(0, 0, tw, th);
        gl_.Disable(GL_DEPTH_TEST);
        gl_.DepthMask(GL_FALSE);
        gl_.Disable(GL_BLEND);
        gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        gl_.BindBuffer(GL_ARRAY_BUFFER, shadow_vbo_);
        if (!hits.empty()) {
            const ShadowDraw &d = shadow_draws_[hits[0].draw];
            gl_.Enable(GL_BLEND);
            gl_.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            gl_.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
            gl_.UseProgram(show);
            gl_.Uniform4f(uniform(show, "u_proj"), p.proj[0], p.proj[1], p.proj[2], p.proj[3]);
            gl_.Uniform2f(uniform(show, "u_proj_z"), p.proj[4], p.proj[5]);
            gl_.Uniform4f(uniform(show, "u_vp"), p.vp[0], p.vp[1], p.vp[2], p.vp[3]);
            gl_.Uniform2f(uniform(show, "u_target"), static_cast<float>(rt.width), static_cast<float>(rt.height));
            gl_.Uniform4f(uniform(show, "u_color"), 1.0f, 1.0f, 1.0f, 0.75f);
            gl_.BindVertexArray(shadow_vao_);
            gl_.DrawArrays(GL_TRIANGLES, static_cast<GLint>(d.first), static_cast<GLsizei>(d.count));
            gl_.Disable(GL_BLEND);
        }
        // A cross at the point looked at.
        gl_.Enable(GL_SCISSOR_TEST);
        gl_.ClearColor(1.0f, 0.0f, 0.0f, 1.0f);
        const GLint mx = static_cast<GLint>(cx) * scale_, my = static_cast<GLint>(cy) * scale_, arm = 5 * scale_;
        gl_.Scissor(mx - arm, my - 1, arm * 2, 2 + scale_ / 2);
        gl_.Clear(GL_COLOR_BUFFER_BIT);
        gl_.Scissor(mx - 1, my - arm, 2 + scale_ / 2, arm * 2);
        gl_.Clear(GL_COLOR_BUFFER_BIT);
        gl_.Disable(GL_SCISSOR_TEST);
        gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    gl_.BindVertexArray(empty_vao_);
}

// Draws the map again for `p`, from the eye space of its picture. The caller's GL state does not survive.
void GlBackend::draw_shadow_map(const ShadowPass &p, const ShadowKept &kept, const std::vector<float> &vertices, bool player, GLuint fbo,
                                bool moving_only) {
    const float half = player ? p.player_half : p.half;
    const float *center = player ? p.player_center : p.center;
    const GLsizei size = player ? kShadowPlayerSize : shadow_size_;
    gl_.Disable(GL_SCISSOR_TEST);
    gl_.Disable(GL_BLEND);
    gl_.Disable(GL_CULL_FACE);
    gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl_.DepthMask(GL_TRUE);
    gl_.Enable(GL_DEPTH_TEST);
    gl_.DepthFunc(GL_LESS);
    gl_.BindFramebuffer(GL_FRAMEBUFFER, fbo != 0 ? fbo : player ? shadow_player_fbo_ : shadow_fbo_[p.map]);
    gl_.Viewport(0, 0, size, size);
    gl_.ClearColor(0, 0, 0, 0);
    if (!moving_only) gl_.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const GLuint program = shadow_map_program_;
    gl_.UseProgram(program);
    gl_.Uniform3f(uniform(program, "u_center"), center[0], center[1], center[2]);
    gl_.Uniform3f(uniform(program, "u_right"), p.right[0], p.right[1], p.right[2]);
    gl_.Uniform3f(uniform(program, "u_up"), p.up[0], p.up[1], p.up[2]);
    gl_.Uniform3f(uniform(program, "u_dir"), p.dir[0], p.dir[1], p.dir[2]);
    gl_.Uniform3f(uniform(program, "u_half"), half, half, half * 3.0f);
    gl_.Uniform1i(uniform(program, "u_tex"), 0);
    // 30 world units with the biggest map: the depth spans 6 half sides, and a coarser map needs more.
    const float coarse = player ? 1.0f : 8192.0f / static_cast<float>(size);
    gl_.Uniform1f(uniform(program, "u_push_limit"), 30.0f * coarse / (6.0f * half));
    const GLint l_xform = uniform(program, "u_uv_xform"), l_func = uniform(program, "u_alpha_func"), l_ref = uniform(program, "u_alpha_ref"),
                l_mask = uniform(program, "u_alpha_mask"), l_s = uniform(program, "u_clamp_s"), l_t = uniform(program, "u_clamp_t"),
                l_scenery = uniform(program, "u_scenery");
    gl_.BindVertexArray(shadow_vao_);
    gl_.BindBuffer(GL_ARRAY_BUFFER, shadow_vbo_);
    gl_.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)), vertices.data(), GL_STREAM_DRAW);
    gl_.ActiveTexture(GL_TEXTURE0);
    for (const KeptRange &r : kept.ranges) {
        if (r.player != player || (moving_only && r.fixed)) continue;
        gl_.Uniform1f(l_scenery, r.scenery ? 1.0f : 0.0f);
        gl_.Uniform1i(l_func, r.texture != 0 ? r.func : 1);
        if (r.texture != 0) {
            gl_.BindTexture(GL_TEXTURE_2D, r.texture);
            gl_.Uniform4f(l_xform, r.xform[0], r.xform[1], r.xform[2], r.xform[3]);
            gl_.Uniform1i(l_ref, r.ref);
            gl_.Uniform1i(l_mask, r.mask);
            gl_.Uniform1i(l_s, r.clamp_s);
            gl_.Uniform1i(l_t, r.clamp_t);
        }
        gl_.DrawArrays(GL_TRIANGLES, static_cast<GLint>(r.first), static_cast<GLsizei>(r.count));
    }
    gl_.BindTexture(GL_TEXTURE_2D, 0);
}

// The shadows into a texture of the picture's size first (the depth cannot be read while it is attached to the target being
// drawn), then multiplied into the picture. `eye_to_map` takes this picture's eye space to the map's: x' = R * (x - t), with
// R's rows in its first nine numbers and t in the last three.
void GlBackend::apply_shadow(RenderTarget &rt, const ShadowPass &p, const float *eye_to_map) {
    const GLsizei full_w = static_cast<GLsizei>(rt.width) * scale_, full_h = static_cast<GLsizei>(rt.height) * scale_;
    // At a small internal resolution there is nothing to spare.
    const int down = scale_ >= 2 && live_->shadow_half_resolution.load(std::memory_order_relaxed) ? 2 : 1;
    const GLsizei w = (full_w + down - 1) / down, h = (full_h + down - 1) / down;
    gl_.Disable(GL_SCISSOR_TEST);
    gl_.Disable(GL_BLEND);
    gl_.Disable(GL_DEPTH_TEST);
    gl_.DepthMask(GL_FALSE);
    gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl_.ActiveTexture(GL_TEXTURE0);
    if (shadow_w_ != w || shadow_h_ != h) {
        if (shadow_factor_fbo_ != 0) {
            gl_.DeleteFramebuffers(1, &shadow_factor_fbo_);
            gl_.DeleteTextures(1, &shadow_factor_tex_);
        }
        gl_.GenTextures(1, &shadow_factor_tex_);
        gl_.BindTexture(GL_TEXTURE_2D, shadow_factor_tex_);
        gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        set_texture_params(false, true, true);
        gl_.GenFramebuffers(1, &shadow_factor_fbo_);
        gl_.BindFramebuffer(GL_FRAMEBUFFER, shadow_factor_fbo_);
        gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shadow_factor_tex_, 0);
        shadow_w_ = w;
        shadow_h_ = h;
    }
    gl_.BindVertexArray(empty_vao_);
    gl_.BindFramebuffer(GL_FRAMEBUFFER, shadow_factor_fbo_);
    gl_.Viewport(0, 0, w, h);
    const GLuint f = shadow_factor_program_;
    gl_.UseProgram(f);
    gl_.ActiveTexture(GL_TEXTURE1);
    gl_.BindTexture(GL_TEXTURE_2D, rt.depth);
    gl_.ActiveTexture(GL_TEXTURE2);
    gl_.BindTexture(GL_TEXTURE_2D, shadow_depth_[p.map]);
    gl_.Uniform1i(uniform(f, "u_depth"), 1);
    gl_.Uniform1i(uniform(f, "u_map"), 2);
    gl_.Uniform1i(uniform(f, "u_kind"), 3);
    gl_.ActiveTexture(GL_TEXTURE3);
    gl_.BindTexture(GL_TEXTURE_2D, shadow_color_[p.map]);
    gl_.Uniform1i(uniform(f, "u_player_map"), 4);
    gl_.ActiveTexture(GL_TEXTURE4);
    gl_.BindTexture(GL_TEXTURE_2D, shadow_player_depth_);
    gl_.Uniform3f(uniform(f, "u_player_center"), p.player_center[0], p.player_center[1], p.player_center[2]);
    gl_.Uniform1f(uniform(f, "u_player_half"), p.player_half);
    gl_.Uniform2f(uniform(f, "u_size"), static_cast<float>(w), static_cast<float>(h));
    gl_.Uniform4f(uniform(f, "u_proj"), p.proj[0], p.proj[1], p.proj[2], p.proj[3]);
    gl_.Uniform2f(uniform(f, "u_proj_z"), p.proj[4], p.proj[5]);
    gl_.Uniform2f(uniform(f, "u_vz"), p.vz[0], p.vz[1]);
    gl_.Uniform4f(uniform(f, "u_vp"), p.vp[0], p.vp[1], p.vp[2], p.vp[3]);
    gl_.Uniform1f(uniform(f, "u_pixel"), static_cast<float>(scale_));
    gl_.Uniform1i(uniform(f, "u_down"), down);
    gl_.Uniform3f(uniform(f, "u_center"), p.center[0], p.center[1], p.center[2]);
    gl_.Uniform3f(uniform(f, "u_right"), p.right[0], p.right[1], p.right[2]);
    gl_.Uniform3f(uniform(f, "u_up"), p.up[0], p.up[1], p.up[2]);
    gl_.Uniform3f(uniform(f, "u_dir"), p.dir[0], p.dir[1], p.dir[2]);
    gl_.Uniform3f(uniform(f, "u_half"), p.half, p.half, p.half * 3.0f);
    gl_.Uniform3f(uniform(f, "u_rc0"), eye_to_map[0], eye_to_map[1], eye_to_map[2]);
    gl_.Uniform3f(uniform(f, "u_rc1"), eye_to_map[3], eye_to_map[4], eye_to_map[5]);
    gl_.Uniform3f(uniform(f, "u_rc2"), eye_to_map[6], eye_to_map[7], eye_to_map[8]);
    gl_.Uniform3f(uniform(f, "u_t"), eye_to_map[9], eye_to_map[10], eye_to_map[11]);
    const int part = live_->shadow_part.load(std::memory_order_relaxed);
    gl_.Uniform1f(uniform(f, "u_darkness"), part == 2 ? 0.0f : p.darkness);
    gl_.Uniform3f(uniform(f, "u_fog"), p.fog[0], p.fog[1], p.fog[2]);
    gl_.Uniform1f(uniform(f, "u_bias"), shadow_bias_);
    gl_.Uniform1f(uniform(f, "u_drop"), p.drop);
    gl_.Uniform1f(uniform(f, "u_soft"), shadow_soft_);
    shadow_time_begin(2);
    gl_.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    shadow_time_end();
    shadow_time_begin(3);
    gl_.ActiveTexture(GL_TEXTURE4);
    gl_.BindTexture(GL_TEXTURE_2D, 0);
    gl_.ActiveTexture(GL_TEXTURE3);
    gl_.BindTexture(GL_TEXTURE_2D, 0);
    gl_.ActiveTexture(GL_TEXTURE1);
    gl_.BindTexture(GL_TEXTURE_2D, 0);
    const auto facing_uniforms = [&](GLuint fp) {
        gl_.Uniform4f(uniform(fp, "u_proj"), p.proj[0], p.proj[1], p.proj[2], p.proj[3]);
        gl_.Uniform2f(uniform(fp, "u_proj_z"), p.proj[4], p.proj[5]);
        gl_.Uniform2f(uniform(fp, "u_vz"), p.vz[0], p.vz[1]);
        gl_.Uniform4f(uniform(fp, "u_vp"), p.vp[0], p.vp[1], p.vp[2], p.vp[3]);
        gl_.Uniform2f(uniform(fp, "u_target"), static_cast<float>(rt.width), static_cast<float>(rt.height));
        gl_.Uniform3f(uniform(fp, "u_rc0"), eye_to_map[0], eye_to_map[1], eye_to_map[2]);
        gl_.Uniform3f(uniform(fp, "u_rc1"), eye_to_map[3], eye_to_map[4], eye_to_map[5]);
        gl_.Uniform3f(uniform(fp, "u_rc2"), eye_to_map[6], eye_to_map[7], eye_to_map[8]);
        gl_.Uniform3f(uniform(fp, "u_t"), eye_to_map[9], eye_to_map[10], eye_to_map[11]);
        gl_.Uniform1f(uniform(fp, "u_darkness"), p.darkness);
        gl_.Uniform3f(uniform(fp, "u_fog"), p.fog[0], p.fog[1], p.fog[2]);
        const float *f = p.facing_motion;
        gl_.Uniform3f(uniform(fp, "u_f0"), f[0], f[1], f[2]);
        gl_.Uniform3f(uniform(fp, "u_f1"), f[3], f[4], f[5]);
        gl_.Uniform3f(uniform(fp, "u_f2"), f[6], f[7], f[8]);
        gl_.Uniform3f(uniform(fp, "u_ft"), f[9], f[10], f[11]);
    };
    if (shadow_facing_count_[p.facing] != 0 && part != 1) {
        // Against the scene's depth, which the shader reads: the darker of the two wins.
        gl_.ActiveTexture(GL_TEXTURE1);
        gl_.BindTexture(GL_TEXTURE_2D, rt.depth);
        gl_.ActiveTexture(GL_TEXTURE0);
        gl_.Enable(GL_BLEND);
        gl_.BlendEquationSeparate(GL_MIN, GL_MIN);
        gl_.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        const GLuint fp = shadow_facing_program_;
        gl_.UseProgram(fp);
        facing_uniforms(fp);
        gl_.Uniform1i(uniform(fp, "u_clear"), 0);
        gl_.Uniform1i(uniform(fp, "u_down"), down);
        gl_.Uniform1i(uniform(fp, "u_depth"), 1);
        gl_.BindVertexArray(shadow_facing_vao_[p.facing]);
        gl_.DrawArrays(GL_TRIANGLES, 0, shadow_facing_count_[p.facing]);
        gl_.BindVertexArray(empty_vao_);
        gl_.Disable(GL_BLEND);
        gl_.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        gl_.ActiveTexture(GL_TEXTURE1);
        gl_.BindTexture(GL_TEXTURE_2D, 0);
        gl_.ActiveTexture(GL_TEXTURE0);
    }
    if (shadow_clear_count_[p.facing] != 0) {
        // The listed pieces of the air write depth, so the map's shadows land on them: those are taken off here. The factor's
        // alpha stays: it says how much to blur.
        const GLuint fp = shadow_facing_program_;
        gl_.UseProgram(fp);
        facing_uniforms(fp);
        gl_.Uniform1i(uniform(fp, "u_clear"), 1);
        gl_.Uniform1i(uniform(fp, "u_down"), down);
        gl_.Uniform1i(uniform(fp, "u_depth"), 1);
        gl_.ActiveTexture(GL_TEXTURE1);
        gl_.BindTexture(GL_TEXTURE_2D, rt.depth);
        gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        gl_.BindVertexArray(shadow_facing_vao_[p.facing]);
        gl_.DrawArrays(GL_TRIANGLES, shadow_facing_count_[p.facing], shadow_clear_count_[p.facing]);
        gl_.BindVertexArray(empty_vao_);
        gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl_.BindTexture(GL_TEXTURE_2D, 0);
        gl_.ActiveTexture(GL_TEXTURE0);
    }

    shadow_time_end();
    gl_.BindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    // The target's depth is read here, so it must not stay attached while it is.
    gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    gl_.Viewport(0, 0, full_w, full_h);
    gl_.UseProgram(shadow_apply_program_);
    shadow_time_begin(4);
    gl_.Uniform1i(uniform(shadow_apply_program_, "u_factor"), 2);
    gl_.Uniform1i(uniform(shadow_apply_program_, "u_depth"), 1);
    gl_.Uniform1i(uniform(shadow_apply_program_, "u_down"), down);
    gl_.Uniform1i(uniform(shadow_apply_program_, "u_step"), std::max(1, scale_ / (2 * down)));
    gl_.ActiveTexture(GL_TEXTURE1);
    gl_.BindTexture(GL_TEXTURE_2D, rt.depth);
    gl_.ActiveTexture(GL_TEXTURE2);
    gl_.BindTexture(GL_TEXTURE_2D, shadow_factor_tex_);
    gl_.Enable(GL_BLEND);
    gl_.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    gl_.BlendFuncSeparate(GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE);
    gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    gl_.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    shadow_time_end();
    gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, rt.depth, 0);
    gl_.Disable(GL_BLEND);
    gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (live_->shadow_show_zone.load(std::memory_order_relaxed)) {
        // "desert 5" from "areas/hunting/desert/area5", drawn into the picture so that the 2D stays over it.
        std::string label = shadow_zone_ >= 0 ? shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] : "no zone";
        for (const char *prefix : {"areas/hunting/", "areas/"}) {
            if (label.rfind(prefix, 0) == 0) label.erase(0, std::strlen(prefix));
        }
        for (char &c : label) c = c == '/' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (const auto at = label.rfind(" AREA"); at != std::string::npos) label.erase(at + 1u, 4u);
        const GLint unit = 2 * scale_ / 2 + (scale_ < 2 ? 1 : 0);
        const auto rect = [&](GLint x, GLint y, GLint width, GLint height) {
            gl_.Scissor(x, y, width, height);
            gl_.Clear(GL_COLOR_BUFFER_BIT);
        };
        const auto write = [&](const std::string &text, GLint x, GLint top) {
            gl_.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            rect(x - unit, top - unit, static_cast<GLint>(text.size()) * 6 * unit + unit, 9 * unit);
            gl_.ClearColor(1.0f, 1.0f, 0.2f, 1.0f);
            for (const char c : text) {
                for (const auto &glyph : kLabelFont) {
                    if (glyph.letter != c) continue;
                    for (int row = 0; row < 7; ++row) {
                        for (int bit = 0; bit < 5; ++bit) {
                            if ((glyph.rows[row] >> (4 - bit) & 1) != 0) rect(x + bit * unit, top + row * unit, unit, unit);
                        }
                    }
                }
                x += 6 * unit;
            }
        };
        gl_.Enable(GL_SCISSOR_TEST);
        write(label + (shadow_night_ ? "  NOCHE" : "  DIA"), 4 * scale_, (static_cast<GLint>(rt.height) - 14) * scale_);
        write(kShadowTestBuild, 4 * scale_, 6 * scale_); // which build is being tested, top left
        if (shadow_darkness_shown_ >= 0.0f) { // the darkness in use, top right (test)
            const std::string dark = "OSCURIDAD " + std::to_string(static_cast<int>(shadow_darkness_shown_ * 100.0f + 0.5f));
            // The picture is 480 wide; the buffer behind it is wider, and its edge is not on the screen.
            write(dark, std::min<GLint>(static_cast<GLint>(rt.width), 480) * scale_ - static_cast<GLint>(dark.size()) * 6 * unit - 6 * scale_, 6 * scale_);
        }
        gl_.Disable(GL_SCISSOR_TEST);
    }
    gl_.BindTexture(GL_TEXTURE_2D, 0);
    gl_.ActiveTexture(GL_TEXTURE1);
    gl_.BindTexture(GL_TEXTURE_2D, 0);
    gl_.ActiveTexture(GL_TEXTURE0);
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
        bool linear = (R(kTexFilter) & 0x100u) != 0 || (live_->smooth_textures.load(std::memory_order_relaxed) && !through);
        if (setup.texture_filter != 0) linear = setup.texture_filter == 2; // a texture pack's [filtering]
        set_texture_params(linear, (R(kTexWrap) & 1u) != 0, (R(kTexWrap) & 0x100u) != 0, setup.texture_mips);
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
                pack_pixel(p, rt.format, row + x * bpp);
            }
        }
    }
    have_state_ = false;
}

void GlBackend::draw_batch() {
    if (batch_.empty() && raw_batch_.empty()) return;
    if (interpolate_) {
        if (tape_.size() >= 20000) { // a game that never flips: do not keep everything
            tape_.clear();
            touched_.insert(~0u);
        }
        // A batch culled whole in this flip is kept for its triangles before clipping: in between it may show.
        tape_.push_back({setup_, batch_, batch_origins_, std::move(raw_batch_), raw_complete_});
    }
    if (!batch_.empty()) {
        const bool haze = setup_.haze || is_camera_haze(setup_.key, batch_);
        const bool shadows_next = shadow_scene_frame_ == frame_ && shadow_done_frame_ != frame_ && live_->shadow.load(std::memory_order_relaxed);
        if (shadows_next && !haze) {
            std::copy_n(shadow_proj_, 6, shadow_scene_view_);
            std::copy_n(shadow_vz_, 2, shadow_scene_view_ + 6);
            std::copy_n(shadow_vp_, 4, shadow_scene_view_ + 8);
        }
        if (shadows_next && haze) {
            if (const auto it = targets_.find(shadow_scene_address_); it != targets_.end() && setup_.target == shadow_scene_address_) {
                // The sheet has a camera of its own: the depth to read is the scene's.
                std::copy_n(shadow_scene_view_, 6, shadow_proj_);
                std::copy_n(shadow_scene_view_ + 6, 2, shadow_vz_);
                std::copy_n(shadow_scene_view_ + 8, 4, shadow_vp_);
                shadow_done_frame_ = frame_;
                run_shadow(it->second);
                gl_.BindVertexArray(vao_);
                gl_.BindBuffer(GL_ARRAY_BUFFER, vbo_);
                have_state_ = false;
                emit_state(setup_, it->second);
            }
        }
        // Test: 3D draws that write no depth (particles, a sandstorm's sand?), counted by texture.
        if (!setup_.through && setup_.tex_hash != 0 && (key_reg(setup_.key, kZWriteDisable) & 1u) != 0 &&
            (key_reg(setup_.key, kAlphaBlendEnable) & 1u) != 0) {
            no_depth_triangles_[setup_.tex_hash] += batch_.size() / 3u;
        }
        // Test: the 2D layers the game lays over the picture, once per zone and texture, to find a sandstorm's.
        if (setup_.through && setup_.tex_hash != 0 && live_->shadow.load(std::memory_order_relaxed)) {
            static std::set<std::pair<int, std::uint64_t>> told;
            if (told.size() < 600u && told.insert({shadow_zone_, setup_.tex_hash}).second) {
                float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
                std::uint8_t most = 0;
                for (const GpuVertex &v : batch_) {
                    lo[0] = std::min(lo[0], v.x), lo[1] = std::min(lo[1], v.y), hi[0] = std::max(hi[0], v.x), hi[1] = std::max(hi[1], v.y);
                    most = std::max(most, v.color[3]);
                }
                const std::string zone = shadow_zone_ >= 0 ? shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)] : std::string("none");
                std::fprintf(stderr, "[shadow 2d] zone %s: texture %016llx, %zu triangles, x %.2f to %.2f, y %.2f to %.2f, blended %u, vertex alpha up to %u\n",
                             zone.c_str(), static_cast<unsigned long long>(setup_.tex_hash), batch_.size() / 3u, lo[0], hi[0], lo[1], hi[1],
                             key_reg(setup_.key, kAlphaBlendEnable) & 1u, static_cast<unsigned>(most));
            }
        }
        const bool effect = shadow_effect(setup_, batch_.data(), batch_.size());
        if (effect) gl_.DepthMask(GL_FALSE);
        draw_vertices(batch_);
        if (effect) gl_.DepthMask(GL_TRUE);
    }
    batch_.clear();
    setup_.largest_draw = 0;
    batch_origins_.clear();
    raw_batch_.clear();
    raw_complete_ = true;
}

// Effects (a weapon's glow, a monster's roar): a few see-through triangles that never get near opaque, written into the
// depth all the same. The shadows are worked out against the depth, so where an effect stood the ground behind it lost its
// shade. Drawn without writing depth they look the same, and the ground keeps it. Not the pieces on the shadows' lists, not
// the characters, and nothing large: veils over the mouths of caves and the foam on the sand take more triangles. Counted by
// draw: an effect is often drawn in a few layers alike, which join in one batch. An effect that fades in and out keeps being
// one at its most opaque: a monster's roar wrote depth there, and its square took the shade of where it stood.
bool GlBackend::shadow_effect(const DrawSetup &setup, const GpuVertex *vertices, std::size_t count) {
    static const bool keep_depth = std::getenv("MHF_SHADOW_EFFECT_DEPTH") != nullptr;
    if (keep_depth || !live_->shadow.load(std::memory_order_relaxed)) return false;
    static std::unordered_set<std::uint64_t> told; // the textures taken for effects
    if (setup.through || setup.haze || setup.listed || setup.texture == 0 || count == 0) return false;
    if ((setup.largest_draw != 0 ? setup.largest_draw : count / 3u) > 16u) return false;
    const auto R = [&](std::uint32_t reg) { return key_reg(setup.key, reg); };
    if ((R(kClearMode) & 1u) != 0 || (R(kAlphaBlendEnable) & 1u) == 0 || (R(kZTestEnable) & 1u) == 0 || (R(kZWriteDisable) & 1u) != 0) return false;
    if (((R(kVertexType) >> 9u) & 3u) != 0) return false; // skinned
    std::uint8_t most = 0;
    for (std::size_t i = 0; i < count; ++i) most = std::max(most, vertices[i].color[3]);
    // An effect fades in and out: once its texture was taken for one, it stays one at its most opaque too (but not in a large batch).
    if (most >= 200 && (told.count(setup.tex_hash) == 0 || count / 3u > 64u)) return false;
    if (told.insert(setup.tex_hash).second && told.size() < 200u) {
        std::fprintf(stderr, "[shadow effect] texture %016llx: %zu triangles (%u in its largest draw), vertex alpha up to %u, drawn without depth\n",
                     static_cast<unsigned long long>(setup.tex_hash), count / 3u, setup.largest_draw, static_cast<unsigned>(most));
    }
    return true;
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

// A rectangle of a render target written into guest RAM at the PSP's resolution, in the target's format: each PSP pixel is
// the average of the scale x scale pixels drawn for it.
void GlBackend::copy_to_ram(RenderTarget &from, std::uint32_t src, std::uint32_t sx, std::uint32_t sy, std::uint32_t dst,
                            std::uint32_t dst_stride, std::uint32_t dx, std::uint32_t dy, std::uint32_t width,
                            std::uint32_t height) {
    const std::uint32_t bpp = from.bytes_per_pixel();
    const std::uint32_t src_offset = ((src & 0x001FFFFFu) - from.address) / bpp;
    const std::uint32_t fx = src_offset % from.stride + sx, fy = src_offset / from.stride + sy;
    if (fx >= from.width || fy >= from.height) return;
    width = std::min(width, from.width - fx);
    height = std::min(height, from.height - fy);
    const std::uint32_t s = static_cast<std::uint32_t>(scale_);
    const std::uint32_t w = width * s, h = height * s;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4u);
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, from.fbo);
    gl_.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl_.ReadPixels(static_cast<GLint>(fx * s), static_cast<GLint>(fy * s), static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                   GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    const std::uint32_t samples = s * s;
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint8_t *row = memory_.raw_pointer(dst + ((dy + y) * dst_stride + dx) * bpp, width * bpp);
        if (row == nullptr) continue;
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint32_t sum[4]{};
            for (std::uint32_t j = 0; j < s; ++j) {
                const std::uint8_t *p = rgba.data() + ((static_cast<std::size_t>(y) * s + j) * w + static_cast<std::size_t>(x) * s) * 4u;
                for (std::uint32_t i = 0; i < s; ++i, p += 4) {
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                }
            }
            const std::uint8_t pixel[4] = {static_cast<std::uint8_t>(sum[0] / samples), static_cast<std::uint8_t>(sum[1] / samples),
                                           static_cast<std::uint8_t>(sum[2] / samples), static_cast<std::uint8_t>(sum[3] / samples)};
            pack_pixel(pixel, from.format, row + x * bpp);
        }
    }
}

void GlBackend::block_transfer(std::uint32_t src, std::uint32_t /*src_stride: the target's own*/, std::uint32_t sx, std::uint32_t sy,
                               std::uint32_t dst, std::uint32_t dst_stride, std::uint32_t dx, std::uint32_t dy,
                               std::uint32_t width, std::uint32_t height, std::uint32_t bpp) {
    draw_batch();
    have_state_ = false; // GL state is changed below
    const bool src_vram = (src & 0x0F000000u) == 0x04000000u, dst_vram = (dst & 0x0F000000u) == 0x04000000u;
    RenderTarget *from = src_vram ? find_target(src & 0x001FFFFFu, true) : nullptr;
    if (!dst_vram) {
        // Into RAM: the GE already copied the bytes, but out of a framebuffer it has drawn they are stale, since the
        // picture only lives in the render target. Read that part back (the quest-end screen keeps a copy of the last
        // hunting picture this way and showed black).
        if (from != nullptr && from->bytes_per_pixel() == bpp) copy_to_ram(*from, src, sx, sy, dst, dst_stride, dx, dy, width, height);
        return;
    }
    const std::uint32_t format = bpp == 4 ? 3u : 1u;
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
    if (++no_depth_pictures_ >= 60u) { // test: about every two seconds, what was drawn in 3D without depth, most first
        std::vector<std::pair<std::size_t, std::uint64_t>> most;
        for (const auto &[hash, n] : no_depth_triangles_) most.emplace_back(n, hash);
        std::sort(most.rbegin(), most.rend());
        std::string line;
        for (std::size_t i = 0; i < most.size() && i < 6u; ++i) {
            char part[64];
            std::snprintf(part, sizeof part, " %016llx:%zu", static_cast<unsigned long long>(most[i].second), most[i].first / no_depth_pictures_);
            line += part;
        }
        std::fprintf(stderr, "[shadow nodepth] zone %s, triangles per picture:%s\n",
                     shadow_zone_ >= 0 ? shadow_zone_names_[static_cast<std::size_t>(shadow_zone_)].c_str() : "none", line.empty() ? " none" : line.c_str());
        no_depth_triangles_.clear();
        no_depth_pictures_ = 0;
    }
    have_state_ = false;
    flush_pictures(); // pictures of the previous flip that never got their turn
    apply_live_scale();
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
    } else {
        // The CPU made this screen, so it is whatever the game wrote: the framebuffer in VRAM (`0x04xxxxxx` memory,
        // the usual case), or an address of the game's own that it flips `sceDisplaySetFrameBuf` between, which is
        // where the movie player draws its pictures (the GE never touches those).
        const std::uint32_t bpp = pixel_format == 3 ? 4u : 2u;
        const std::uint32_t minimum_stride = (address & 0x0F000000u) == 0x04000000u || (address & 0x0F000000u) == 0x08000000u
                                                 ? 480u
                                                 : 512u;
        stride = std::max(stride, minimum_stride);
        const std::size_t needed = static_cast<std::size_t>(stride) * 271u * bpp + 480u * bpp;
        if (memory_.contains(address, needed)) {
            pixels_.resize(480u * 272u);
            const std::uint8_t *base = memory_.raw_pointer(address, needed);
            for (std::uint32_t y = 0; y < 272; ++y) {
                for (std::uint32_t x = 0; x < 480; ++x) {
                    pixels_[y * 480u + x] = framebuffer_pixel(base + (y * stride + x) * bpp, pixel_format);
                }
            }
            gl_.BindTexture(GL_TEXTURE_2D, display_texture_);
            gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 480, 272, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
            texture = display_texture_;
        }
    }

    if (interpolate_) {
        if (drawn == nullptr || !present_interpolated(*drawn, uv_max)) show(texture, uv_max[0], uv_max[1]);
        if (drawn == nullptr) previous_valid_ = false;
    } else {
        show(texture, uv_max[0], uv_max[1]);
    }
    copy_shown_to_vram(drawn);
    finish_flip();
    apply_live_fps(); // after the flip: the next one is the first recorded for the new rate
}

// Some games read the picture on screen with the CPU: Freedom Unite turns the last hunting picture into the background
// of the quest rewards that way. The GE draws into render targets at the internal resolution and never into guest
// VRAM, so the picture shown at each flip is also copied back into its framebuffer at the PSP's 480x272. It is read
// back through a pixel buffer once the GPU has finished with it (a fence says when), so drawing never waits for it. Each
// framebuffer is copied every fourth flip at most, which keeps the cost low on a slow GPU; a game reading the screen
// finds a picture from a few flips before (a tenth of a second or so), which is what was on display.
void GlBackend::copy_shown_to_vram(RenderTarget *shown) {
    constexpr std::uint32_t kWidth = 480, kHeight = 272;
    if (pending_readback_.active) {
        // Not read back yet (a slow GPU, or a software one): try again at the next flip rather than wait here.
        if (gl_.ClientWaitSync(pending_readback_.fence, 0, 0) == GL_TIMEOUT_EXPIRED) return;
        gl_.DeleteSync(pending_readback_.fence);
        const PendingReadback p = pending_readback_;
        pending_readback_.active = false;
        gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, readback_pbo_);
        const auto *rgba = static_cast<const std::uint8_t *>(
            gl_.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(kWidth * kHeight * 4u), GL_MAP_READ_BIT));
        if (rgba != nullptr) {
            const std::uint32_t bpp = p.format == 3 ? 4u : 2u;
            for (std::uint32_t y = 0; y < p.height; ++y) {
                std::uint8_t *row = memory_.raw_pointer(0x04000000u + p.address + y * p.stride * bpp, p.width * bpp);
                if (row == nullptr) continue;
                const std::uint8_t *line = rgba + static_cast<std::size_t>(y) * p.width * 4u;
                if (p.format == 3) {
                    std::memcpy(row, line, p.width * 4u); // the same bytes as RGBA8
                    continue;
                }
                for (std::uint32_t x = 0; x < p.width; ++x) pack_pixel(line + x * 4u, p.format, row + x * bpp);
            }
            gl_.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    // Each framebuffer is copied at most every fourth flip. Counting flips instead would copy only one framebuffer of a
    // double-buffered game (they take turns), and the game would read the other one black.
    if (shown == nullptr || (shown->copied != 0 && frame_ + 1 - shown->copied < 4)) return;
    shown->copied = frame_ + 1;
    if (readback_fbo_ == 0) {
        gl_.GenTextures(1, &readback_texture_);
        gl_.BindTexture(GL_TEXTURE_2D, readback_texture_);
        gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kWidth, kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl_.GenFramebuffers(1, &readback_fbo_);
        gl_.BindFramebuffer(GL_FRAMEBUFFER, readback_fbo_);
        gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, readback_texture_, 0);
        gl_.GenBuffers(1, &readback_pbo_);
        gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, readback_pbo_);
        gl_.BufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(kWidth * kHeight * 4u), nullptr, GL_STREAM_READ);
        gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    const std::uint32_t width = std::min(kWidth, shown->width), height = std::min(kHeight, shown->height);
    const GLint s = scale_;
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, shown->fbo);
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, readback_fbo_);
    gl_.Disable(GL_SCISSOR_TEST);
    gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl_.BlitFramebuffer(0, 0, static_cast<GLint>(width) * s, static_cast<GLint>(height) * s, 0, 0, static_cast<GLint>(width),
                        static_cast<GLint>(height), GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, readback_fbo_);
    gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, readback_pbo_);
    gl_.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl_.ReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    pending_readback_ = {true, gl_.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), shown->address, shown->stride, shown->format, width,
                         height};
    have_state_ = false;
}

// Puts a picture on the window: letterboxed to the PSP's 480:272, with the dialog overlay on top.
void GlBackend::show(GLuint texture, float uv_max_x, float uv_max_y) {
    const auto show_start = std::chrono::steady_clock::now();
    last_texture_ = texture;
    last_uv_[0] = uv_max_x;
    last_uv_[1] = uv_max_y;
    last_show_ = show_start;
    int window_w = 0, window_h = 0;
    SDL_GetWindowSizeInPixels(window_, &window_w, &window_h);
    gl_.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_.Disable(GL_SCISSOR_TEST);
    gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl_.Viewport(0, 0, window_w, window_h);
    gl_.ClearColor(0, 0, 0, 1);
    gl_.Clear(GL_COLOR_BUFFER_BIT);
    bool menu = false;
    if (overlay_ != nullptr) {
        std::lock_guard lock(overlay_->mutex);
        menu = overlay_->continuous;
    }
    if (menu && texture != 0 && window_w > 0 && window_h > 0) {
        // The menu is open: the whole window shows the game running behind it, blurred and darkened.
        if (!blur_ready_ || blur_source_ != texture || !redraw_only_) build_blur(texture, uv_max_x, uv_max_y);
        draw_blurred_background(window_w, window_h);
        have_state_ = false;
    } else if (texture != 0 && window_w > 0 && window_h > 0) {
        blur_ready_ = false;
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
    const auto swap_start = std::chrono::steady_clock::now();
    SDL_GL_SwapWindow(window_);
    const auto swap_end = std::chrono::steady_clock::now();
    cost_.swap_us += std::chrono::duration<double, std::micro>(swap_end - swap_start).count();
    cost_.show_us += std::chrono::duration<double, std::micro>(swap_end - show_start).count();
    ++cost_.shows;
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

    seen_v_ = nullptr;
    pack_uploads_ = 0;
    trim_pack_memory();
    if (pack_log_ && loader_ && frame_ >= pack_logged_at_ + 300) {
        pack_logged_at_ = frame_;
        const auto c = loader_->counters();
        std::fprintf(stderr, "[textures] %llu images on the GPU (%zu now, %llu reused), %llu not in the pack; %llu images decoded (%.1f ms each), %llu failed; %zu MB on the GPU\n",
                     static_cast<unsigned long long>(pack_replaced_), pack_images_.size(), static_cast<unsigned long long>(pack_reused_),
                     static_cast<unsigned long long>(pack_missed_),
                     static_cast<unsigned long long>(c.decoded), c.decoded + c.failed != 0 ? c.decode_us / 1000.0 / double(c.decoded + c.failed) : 0.0,
                     static_cast<unsigned long long>(c.failed), pack_bytes_ >> 20);
    }

    // Forget textures not used for a while.
    if ((frame_ & 255u) == 0) {
        for (auto i = textures_.begin(); i != textures_.end();) {
            if (i->second.last_use + 600 < frame_) {
                if (i->second.shown.empty()) gl_.DeleteTextures(1, &i->second.texture); // a pack image's stays with it
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

// Interpolation v2: the batch's triangles before clipping, each moved by its draw's transform at this moment
// (v2_transforms_), clipped against the near plane, dropped when a point leaves the GE's 4096x4096 space, and placed on
// the screen through its draw's viewport, as Ge::draw_triangle does for the flip itself.
// Runs `work(from, to)` over [0, count) in shares of at least `least` items: on the drawing thread alone when small,
// else split with up to three helper threads.
template <typename Work> void in_shares(std::size_t count, std::size_t least, const Work &work) {
    const unsigned cores = std::thread::hardware_concurrency();
    std::size_t lanes = cores >= 3 ? std::min<std::size_t>(4, cores - 1) : 1;
    lanes = std::max<std::size_t>(1, std::min(lanes, count / std::max<std::size_t>(1, least)));
    if (lanes <= 1) {
        work(0, count);
        return;
    }
    const std::size_t share = (count + lanes - 1) / lanes;
    std::vector<std::future<void>> helpers;
    for (std::size_t k = 1; k < lanes; ++k) {
        const std::size_t from = std::min(count, k * share), to = std::min(count, (k + 1) * share);
        helpers.push_back(std::async(std::launch::async, [&work, from, to] { work(from, to); }));
    }
    work(0, std::min(count, share));
    for (auto &helper : helpers) helper.get();
}

// Interpolation v2, once a flip: per draw what its transform needs whatever the moment, and the shown picture's 3D
// batches cut into shares of about the same number of triangles.
void GlBackend::prepare_v2(const std::vector<TapeEntry> &now, const std::vector<TapeEntry> &before) {
    v2_prepared_ = true;
    const std::vector<interpolation::DrawSummary> &newer = summaries_before_;
    // Each draw's triangles in both flips, in drawing order (a draw's triangles follow one another across batches).
    v2_old_tris_.clear();
    v2_old_first_.assign(v2_older_.size(), 0);
    v2_old_count_.assign(v2_older_.size(), 0);
    for (const TapeEntry &e : before) {
        if (e.setup.through) continue;
        for (const RawTriangle &tri : e.raw) {
            if (tri.draw >= v2_older_.size()) continue;
            if (v2_old_count_[tri.draw]++ == 0) v2_old_first_[tri.draw] = v2_old_tris_.size();
            v2_old_tris_.push_back(&tri);
        }
    }
    v2_new_first_.assign(newer.size(), 0);
    v2_new_count_.assign(newer.size(), 0);
    v2_batch_ordinal_.assign(now.size(), 0);
    std::size_t ordinal = 0;
    for (std::size_t i = 0; i < now.size(); ++i) {
        v2_batch_ordinal_[i] = ordinal;
        if (now[i].setup.through) continue;
        for (const RawTriangle &tri : now[i].raw) {
            if (tri.draw < newer.size() && v2_new_count_[tri.draw]++ == 0) v2_new_first_[tri.draw] = ordinal;
            ++ordinal;
        }
    }
    v2_draws_.resize(newer.size()); // only the needed draws are read, and each is written whole below
    std::vector<std::uint8_t> needed(newer.size(), 0);
    v2_chunks_.clear();
    v2_first_chunk_.assign(now.size(), 0);
    constexpr std::size_t kChunk = 768;
    for (std::size_t i = 0; i < now.size(); ++i) {
        const TapeEntry &e = now[i];
        v2_first_chunk_[i] = v2_chunks_.size();
        if (e.setup.through || !e.raw_complete) continue;
        for (const RawTriangle &tri : e.raw) {
            if (tri.draw < needed.size()) needed[tri.draw] = 1;
        }
        for (std::size_t from = 0; from < e.raw.size(); from += kChunk) v2_chunks_.push_back({i, from, std::min(e.raw.size(), from + kChunk)});
    }
    v2_chunk_out_.resize(v2_chunks_.size());
    v2_needed_.clear();
    for (std::size_t n = 0; n < newer.size(); ++n) {
        if (needed[n] != 0) v2_needed_.push_back(static_cast<std::uint32_t>(n));
    }
    in_shares(v2_needed_.size(), 256, [&](std::size_t first, std::size_t last) {
        for (std::size_t k = first; k < last; ++k) prepare_v2_draw(v2_needed_[k]);
    });
}

void GlBackend::prepare_v2_draw(std::uint32_t n) {
    {
        const interpolation::DrawSummary &d = summaries_before_[n];
        V2Draw &out = v2_draws_[n];
        out = V2Draw{};
        out.fixed = interpolation::multiply(d.projection, d.view); // as drawn
        const interpolation::Matrix eye = interpolation::multiply(d.view, d.world);
        interpolation::Matrix eye_inverse{};
        if (!interpolation::affine_inverse(eye, eye_inverse)) return;
        // A point in the world (after the world matrix) is in eye space through the view; there, eye_inverse takes it
        // back to the model.
        out.back = interpolation::multiply(eye_inverse, d.view);
        out.projection_new = d.projection;
        const std::int32_t o = n < v2_older_of_.size() ? v2_older_of_[n] : -1;
        if (o >= 0 && static_cast<std::size_t>(o) < v2_older_.size()) {
            const interpolation::DrawSummary &from = v2_older_[static_cast<std::size_t>(o)];
            out.older_eye = interpolation::multiply(from.view, from.world);
            out.projection_old = from.projection;
            const std::size_t oi = static_cast<std::size_t>(o);
            interpolation::Matrix older_inverse{};
            if (d.skinned && v2_new_count_[n] > 0 && v2_new_count_[n] == v2_old_count_[oi] &&
                interpolation::affine_inverse(out.older_eye, older_inverse)) {
                out.deform = true;
                out.old_first = v2_old_first_[oi];
                out.new_first = v2_new_first_[n];
                out.back_old = interpolation::multiply(older_inverse, from.view);
            }
            if (out.older_eye == eye) {
                // Fixed in eye space (interpolation::blend_eye keeps it): at most the projection changes.
                if (from.projection == d.projection && !out.deform) return;
                out.kind = V2Draw::kStill;
                out.own = eye;
                return;
            }
            out.kind = V2Draw::kBlend;
            // The newer transform as the older camera would have seen it (interpolation::blend_eye).
            out.own = v2_camera_.valid ? interpolation::multiply(v2_camera_.inverse, eye) : eye;
        } else if (v2_camera_.valid && d.eligible) {
            // No partner (drawn only in this flip): it moves with the camera only.
            out.kind = V2Draw::kFollow;
            out.own = interpolation::multiply(v2_camera_.inverse, eye);
        }
    }
}

void GlBackend::place_v2(std::size_t batch, const TapeEntry &entry, std::size_t from, std::size_t to,
                         std::vector<GpuVertex> &out) const {
    struct Clip {
        float x, y, z, w;
        const RawVertex *r;
        float u, v, fog, color[4], spec[3];
    };
    const auto lerp = [](const Clip &p, const Clip &q, float s) {
        Clip c = p;
        const auto l = [s](float a, float b) { return a + (b - a) * s; };
        c.x = l(p.x, q.x); c.y = l(p.y, q.y); c.z = l(p.z, q.z); c.w = l(p.w, q.w);
        c.u = l(p.u, q.u); c.v = l(p.v, q.v); c.fog = l(p.fog, q.fog);
        for (int k = 0; k < 4; ++k) c.color[k] = l(p.color[k], q.color[k]);
        for (int k = 0; k < 3; ++k) c.spec[k] = l(p.spec[k], q.spec[k]);
        return c;
    };
    out.reserve((to - from) * 3);
    for (std::size_t k = from; k < to; ++k) {
        const RawTriangle &tri = entry.raw[k];
        if (tri.draw >= v2_transforms_.size() || tri.draw >= viewports_now_.size()) continue;
        const interpolation::Matrix &m = v2_transforms_[tri.draw];
        const std::array<float, 8> &vp = viewports_now_[tri.draw];
        float cx[3], cy[3], cz[3], cw[3];
        bool all_in = true;
        const V2Draw &draw = v2_draws_[tri.draw];
        const RawTriangle *was = nullptr;
        if (draw.deform) {
            const std::size_t index = v2_batch_ordinal_[batch] + k - draw.new_first;
            if (draw.old_first + index < v2_old_tris_.size()) was = v2_old_tris_[draw.old_first + index];
        }
        for (int i = 0; i < 3; ++i) {
            const RawVertex &r = tri.v[i];
            float x = r.world[0], y = r.world[1], z = r.world[2];
            if (was != nullptr) {
                // Both points in the model, blended, then on through this moment's eye and projection.
                const interpolation::Matrix &bn = draw.back, &bo = draw.back_old;
                const float *w = was->v[i].world;
                float model[3];
                for (int row = 0; row < 3; ++row) {
                    const float now_at = bn[row] * x + bn[4 + row] * y + bn[8 + row] * z + bn[12 + row];
                    const float then_at = bo[row] * w[0] + bo[4 + row] * w[1] + bo[8 + row] * w[2] + bo[12 + row];
                    model[row] = then_at + (now_at - then_at) * v2_t_;
                }
                const interpolation::Matrix &vt = v2_view_t_[tri.draw];
                cx[i] = vt[0] * model[0] + vt[4] * model[1] + vt[8] * model[2] + vt[12];
                cy[i] = vt[1] * model[0] + vt[5] * model[1] + vt[9] * model[2] + vt[13];
                cz[i] = vt[2] * model[0] + vt[6] * model[1] + vt[10] * model[2] + vt[14];
                cw[i] = vt[3] * model[0] + vt[7] * model[1] + vt[11] * model[2] + vt[15];
                all_in = all_in && cz[i] > -cw[i] && cw[i] > 0.0f;
                continue;
            }
            cx[i] = m[0] * x + m[4] * y + m[8] * z + m[12];
            cy[i] = m[1] * x + m[5] * y + m[9] * z + m[13];
            cz[i] = m[2] * x + m[6] * y + m[10] * z + m[14];
            cw[i] = m[3] * x + m[7] * y + m[11] * z + m[15];
            all_in = all_in && cz[i] > -cw[i] && cw[i] > 0.0f;
        }
        if (all_in) {
            // The common case: in front of the near plane; straight to the screen, or dropped past the guard band.
            GpuVertex g[3];
            bool outside = false;
            for (int i = 0; i < 3 && !outside; ++i) {
                const RawVertex &r = tri.v[i];
                const float inv_w = 1.0f / cw[i];
                const float sx = cx[i] * inv_w * vp[0] + vp[3], sy = cy[i] * inv_w * vp[1] + vp[4];
                outside = !(sx >= 0.0f && sx < 4096.0f && sy >= 0.0f && sy < 4096.0f);
                g[i].x = sx - vp[6];
                g[i].y = sy - vp[7];
                g[i].z = cz[i] * inv_w * vp[2] + vp[5];
                g[i].inv_w = inv_w;
                g[i].u = r.u;
                g[i].v = r.v;
                std::memcpy(g[i].color, r.color, 4);
                std::memcpy(g[i].spec, r.spec, 4);
                g[i].fog = r.fog;
            }
            if (!outside) out.insert(out.end(), g, g + 3);
            continue;
        }
        Clip in[3];
        for (int i = 0; i < 3; ++i) {
            const RawVertex &r = tri.v[i];
            Clip &c = in[i];
            c.x = cx[i];
            c.y = cy[i];
            c.z = cz[i];
            c.w = cw[i];
            c.r = &r;
            c.u = r.u;
            c.v = r.v;
            c.fog = r.fog;
            for (int k = 0; k < 4; ++k) c.color[k] = r.color[k];
            for (int k = 0; k < 3; ++k) c.spec[k] = r.spec[k];
        }
        const auto to_screen = [&](const Clip &c, GpuVertex &g) {
            const float inv_w = c.w != 0.0f ? 1.0f / c.w : 1.0f;
            g.x = c.x * inv_w * vp[0] + vp[3] - vp[6];
            g.y = c.y * inv_w * vp[1] + vp[4] - vp[7];
            g.z = c.z * inv_w * vp[2] + vp[5];
            g.inv_w = inv_w > 0.0f ? inv_w : 1.0f;
            g.u = c.u;
            g.v = c.v;
            for (int k = 0; k < 4; ++k) g.color[k] = to_byte(c.color[k]);
            for (int k = 0; k < 3; ++k) g.spec[k] = to_byte(c.spec[k]);
            g.spec[3] = 0;
            g.fog = c.fog;
        };
        // The guard band, for the points in front of the near plane.
        bool any_clipped = false, outside = false;
        for (const Clip &c : in) {
            if (!(c.z > -c.w && c.w > 0.0f)) {
                any_clipped = true;
                continue;
            }
            const float inv_w = 1.0f / c.w;
            const float sx = c.x * inv_w * vp[0] + vp[3], sy = c.y * inv_w * vp[1] + vp[4];
            if (!(sx >= 0.0f && sx < 4096.0f && sy >= 0.0f && sy < 4096.0f)) {
                outside = true;
                break;
            }
        }
        if (outside) continue;
        if (!any_clipped) {
            for (const Clip &c : in) {
                GpuVertex g{};
                to_screen(c, g);
                out.push_back(g);
            }
            continue;
        }
        Clip poly[4];
        int n = 0;
        for (int i = 0; i < 3; ++i) {
            const Clip &p = in[i], &q = in[(i + 1) % 3];
            const float dp = p.z + p.w, dq = q.z + q.w;
            const bool pin = dp > 0.0f && p.w > 0.0f, qin = dq > 0.0f && q.w > 0.0f;
            if (pin) poly[n++] = p;
            if (pin != qin && dp != dq) {
                const Clip c = lerp(p, q, dp / (dp - dq));
                if (c.w <= 0.0f) continue;
                poly[n++] = c;
            }
        }
        if (n < 3) continue;
        GpuVertex s[4]{};
        for (int i = 0; i < n; ++i) to_screen(poly[i], s[i]);
        out.push_back(s[0]);
        out.push_back(s[1]);
        out.push_back(s[2]);
        if (n == 4) {
            out.push_back(s[0]);
            out.push_back(s[2]);
            out.push_back(s[3]);
        }
    }
}

void GlBackend::render_between(const RenderTarget &shown, const std::vector<TapeEntry> &now,
                               const std::vector<TapeEntry> &before_batches, const Slot &before_slot, float t) {
    const GLsizei w = static_cast<GLsizei>(shown.width) * scale_, h = static_cast<GLsizei>(shown.height) * scale_;
    if (scratch_.fbo == 0 || scratch_.width != shown.width || scratch_.height != shown.height) {
        if (scratch_.fbo != 0) {
            gl_.DeleteFramebuffers(1, &scratch_.fbo);
            gl_.DeleteTextures(1, &scratch_.color);
            gl_.DeleteTextures(1, &scratch_.depth);
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
    // Interpolation v2: the 3D batches are made again from their triangles before clipping, moved with their draw's
    // transforms blended to this moment, then clipped and placed as the GE does (place_v2).
    const bool v2 = v2_enabled_ && v2_ready_ && !summaries_before_.empty();
    interpolation::Matrix camera_at{}; // the camera's motion up to this moment, for the shadows
    interpolation::Matrix scene_motion{}; // how the scenery itself moved, from the flip to this moment
    interpolation::Matrix scene_whole{};  // and over the whole flip, from the one before
    bool scene_moved = false;
    if (v2) {
        if (!v2_prepared_) prepare_v2(now, before_batches);
        v2_t_ = t;
        // The camera's motion this far, once; then each draw's transform, then the triangles, in even shares.
        const interpolation::Matrix camera_t = interpolation::rigid_at(v2_camera_, t);
        camera_at = camera_t;
        v2_transforms_.resize(v2_draws_.size());
        v2_view_t_.resize(v2_draws_.size());
        const auto transforms = [&](std::size_t from, std::size_t to) {
            for (std::size_t k = from; k < to; ++k) {
                const std::uint32_t n = v2_needed_[k];
                const V2Draw &d = v2_draws_[n];
                if (d.kind == V2Draw::kFixed) {
                    v2_transforms_[n] = d.fixed;
                    continue;
                }
                interpolation::Matrix eye_t;
                switch (d.kind) {
                case V2Draw::kStill: eye_t = d.own; break;
                case V2Draw::kFollow: eye_t = interpolation::multiply(camera_t, d.own); break;
                default:
                    eye_t = interpolation::blend_affine(d.older_eye, d.own, t);
                    if (v2_camera_.valid) eye_t = interpolation::multiply(camera_t, eye_t);
                    break;
                }
                const interpolation::Matrix projection_t =
                    d.kind == V2Draw::kFollow ? d.projection_new : interpolation::blend_linear(d.projection_old, d.projection_new, t);
                const interpolation::Matrix view_t = interpolation::multiply(projection_t, eye_t);
                if (d.deform) v2_view_t_[n] = view_t;
                v2_transforms_[n] = interpolation::multiply(view_t, d.back);
            }
        };
        const auto chunks = [&](std::size_t from, std::size_t to) {
            for (std::size_t k = from; k < to; ++k) {
                const V2Chunk &c = v2_chunks_[k];
                v2_chunk_out_[k].clear();
                place_v2(c.batch, now[c.batch], c.from, c.to, v2_chunk_out_[k]);
            }
        };
        in_shares(v2_needed_.size(), 512, transforms);
        in_shares(v2_chunks_.size(), 1, chunks);
        // The shadows must follow the ground as it is drawn in this picture. The camera found by the votes of all the draws can
        // differ from that (in some places monsters or plants outvote it), and the shadows would slide over the ground and back.
        // A single piece can be wrong too (a thing that moves, or a piece paired with its neighbour), so the biggest pieces vote,
        // by their triangles, standing still included. Everything built on the ground moves alike.
        v2_scene_votes_.clear();
        for (const std::uint32_t n : v2_needed_) {
            if (!summaries_before_[n].skinned && n < v2_new_count_.size() && v2_new_count_[n] != 0) v2_scene_votes_.push_back(n);
        }
        const std::size_t voters = std::min<std::size_t>(v2_scene_votes_.size(), 48u);
        std::partial_sort(v2_scene_votes_.begin(), v2_scene_votes_.begin() + static_cast<std::ptrdiff_t>(voters), v2_scene_votes_.end(),
                          [&](std::uint32_t x, std::uint32_t y) { return v2_new_count_[x] > v2_new_count_[y]; });
        struct SceneVote {
            interpolation::Matrix at, whole;
            std::size_t weight;
            bool ok;
        };
        std::vector<SceneVote> votes(voters);
        for (std::size_t i = 0; i < voters; ++i) {
            const std::uint32_t n = v2_scene_votes_[i];
            const V2Draw &d = v2_draws_[n];
            const interpolation::DrawSummary &summary = summaries_before_[n];
            const interpolation::Matrix eye = interpolation::multiply(summary.view, summary.world);
            interpolation::Matrix eye_t = eye, eye_inverse{}, older_inverse{};
            if (d.kind == V2Draw::kFollow) eye_t = interpolation::multiply(camera_t, d.own);
            if (d.kind == V2Draw::kBlend) {
                eye_t = interpolation::blend_affine(d.older_eye, d.own, t);
                if (v2_camera_.valid) eye_t = interpolation::multiply(camera_t, eye_t);
            }
            SceneVote &v = votes[i];
            v.weight = v2_new_count_[n];
            v.ok = interpolation::affine_inverse(eye, eye_inverse);
            if (!v.ok) continue;
            v.at = interpolation::multiply(eye_t, eye_inverse);
            v.whole = interpolation::multiply(eye, eye_inverse);
            if (d.kind == V2Draw::kFollow) v.whole = interpolation::rigid_at(v2_camera_, 1.0f);
            if (d.kind == V2Draw::kBlend && interpolation::affine_inverse(d.older_eye, older_inverse)) {
                v.whole = interpolation::multiply(eye, older_inverse);
            }
        }
        const auto same = [](const interpolation::Matrix &p, const interpolation::Matrix &q) {
            static constexpr float probes[3][3] = {{0, 0, -500}, {1000, 0, -3000}, {0, 1000, -3000}};
            for (const auto &o : probes) {
                float gap = 0.0f;
                for (std::size_t k = 0; k < 3; ++k) {
                    const float x = p[k] * o[0] + p[4 + k] * o[1] + p[8 + k] * o[2] + p[12 + k];
                    const float y = q[k] * o[0] + q[4 + k] * o[1] + q[8 + k] * o[2] + q[12 + k];
                    gap += (x - y) * (x - y);
                }
                if (gap > 9.0f) return false;
            }
            return true;
        };
        std::size_t best_weight = 0, total_weight = 0;
        for (std::size_t i = 0; i < voters; ++i) {
            if (!votes[i].ok) continue;
            total_weight += votes[i].weight;
            std::size_t weight = 0;
            for (std::size_t j = 0; j < voters; ++j) {
                if (votes[j].ok && same(votes[i].at, votes[j].at)) weight += votes[j].weight;
            }
            if (weight > best_weight) {
                best_weight = weight;
                scene_motion = votes[i].at;
                scene_whole = votes[i].whole;
                scene_moved = true;
            }
        }
        shadow_scene_share_ += total_weight != 0 ? static_cast<double>(best_weight) / static_cast<double>(total_weight) : 0.0;
        ++shadow_scene_pictures_;
    }
    staging_.clear();
    ranges_.assign(now.size(), Range{});
    for (std::size_t i = 0; i < now.size(); ++i) {
        const TapeEntry &entry = now[i];
        const bool made = v2 && !entry.setup.through && entry.raw_complete;
        if (made) {
            const std::size_t first = staging_.size();
            for (std::size_t k = v2_first_chunk_[i]; k < v2_chunks_.size() && v2_chunks_[k].batch == i; ++k) {
                staging_.insert(staging_.end(), v2_chunk_out_[k].begin(), v2_chunk_out_[k].end());
            }
            ranges_[i] = Range{first, staging_.size() - first};
            continue;
        }
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
            } else if (!(v2 && entry.raw_complete)) {
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
    // Projected shadows, as in the flip's own picture: once, after the 3D scene and before the 2D drawing over it. The flip's
    // map holds while at most one newer one was drawn (there are two).
    bool shadow_pending = flip_.shadow.valid && shadow_ready_ && live_->shadow.load(std::memory_order_relaxed) &&
                          shadow_generation_ - flip_.shadow.generation < 2u;
    bool scene_drawn = false;
    for (std::size_t i = 0; i < now.size(); ++i) {
        if (ranges_[i].count == 0) continue;
        if (shadow_pending && (key_reg(now[i].setup.key, kClearMode) & 1u) == 0) {
            const bool haze = now[i].setup.haze || is_camera_haze(now[i].setup.key, now[i].vertices);
            if (!now[i].setup.through && !haze) {
                if ((key_reg(now[i].setup.key, kZTestEnable) & 1u) != 0 && (key_reg(now[i].setup.key, kZWriteDisable) & 1u) == 0) {
                    scene_drawn = true;
                }
            } else if (scene_drawn) {
                shadow_pending = false;
                // A fixed point has the eye coordinates v_t = C * v_flip, with C = (the camera's motion up to t) * (its motion
                // over the whole flip, undone): the pixel looks up the map through v_flip = C^-1 * v_t.
                float eye_to_map[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
                if (v2 && (scene_moved || v2_camera_.valid)) {
                    const interpolation::Matrix c = scene_moved ? scene_motion : interpolation::multiply(camera_at, v2_camera_.inverse);
                    for (int k = 0; k < 3; ++k) {
                        for (int j = 0; j < 3; ++j) eye_to_map[3 * k + j] = c[static_cast<std::size_t>(4 * k + j)];
                        eye_to_map[9 + k] = c[static_cast<std::size_t>(12 + k)];
                    }
                }
                // The player moves between flips: its small map is drawn again with it where it is at this moment, from the same
                // points of the flip before, brought into this flip's eye space. Everything else stays as the flip mapped it.
                const ShadowKept &kept = shadow_kept_[flip_.shadow.generation % 3u];
                const ShadowKept &older = shadow_kept_[(flip_.shadow.generation + 2u) % 3u];
                if (v2 && (scene_moved || v2_camera_.valid) && flip_.shadow.player_half > 0.0f && kept.generation == flip_.shadow.generation) {
                    shadow_between_ = kept.vertices;
                    if (older.generation + 1u == kept.generation) {
                        // By the same motion as the rest of the shadows, or the hunter's would jump between its two places.
                        const interpolation::Matrix motion = scene_moved ? scene_whole : interpolation::rigid_at(v2_camera_, 1.0f);
                        for (const KeptRange &o : older.ranges) o.paired = false;
                        for (const KeptRange &r : kept.ranges) {
                            if (!r.player) continue;
                            // The same model: characters come and go from what is drawn, so the order says nothing.
                            const auto from = std::find_if(older.ranges.begin(), older.ranges.end(), [&r](const KeptRange &o) {
                                return !o.paired && o.vertices == r.vertices && o.count == r.count;
                            });
                            if (from == older.ranges.end()) continue;
                            from->paired = true;
                            for (std::uint32_t v = 0; v < r.count; ++v) {
                                float *out = shadow_between_.data() + (static_cast<std::size_t>(r.first) + v) * 5u;
                                const float *in = older.vertices.data() + (static_cast<std::size_t>(from->first) + v) * 5u;
                                for (int k = 0; k < 3; ++k) {
                                    const float old_here = motion[static_cast<std::size_t>(k)] * in[0] + motion[static_cast<std::size_t>(4 + k)] * in[1] +
                                                           motion[static_cast<std::size_t>(8 + k)] * in[2] + motion[static_cast<std::size_t>(12 + k)];
                                    out[k] = old_here + (out[k] - old_here) * t;
                                }
                            }
                        }
                    }
                    shadow_time_begin(1);
                    draw_shadow_map(flip_.shadow, kept, shadow_between_, true);
                    shadow_time_end();
                }
                apply_shadow(scratch_, flip_.shadow, eye_to_map);
                gl_.BindVertexArray(vao_);
                gl_.BindBuffer(GL_ARRAY_BUFFER, vbo_);
                have_state_ = false;
            }
        }
        const auto t_state = Clock::now();
        emit_state(now[i].setup, scratch_);
        const bool effect = shadow_effect(now[i].setup, staging_.data() + ranges_[i].first, ranges_[i].count);
        if (effect) gl_.DepthMask(GL_FALSE);
        const auto t_draw = Clock::now();
        gl_.DrawArrays(GL_TRIANGLES, static_cast<GLint>(ranges_[i].first), static_cast<GLsizei>(ranges_[i].count));
        if (effect) gl_.DepthMask(GL_TRUE);
        cost_.state_us += us(t_state, t_draw);
        cost_.draw_us += us(t_draw, Clock::now());
    }
    ++cost_.pictures;
    cost_.batches += now.size();
    cost_.vertices += staging_.size();
    have_state_ = false;
}

// Where the shown picture is between the previous flip (0) and this one (1) at `when`. Pictures run one step behind
// the game: the first one, drawn as the flip arrives, is already one step in, and the flip's own picture is due one
// step before the next flip is.
float GlBackend::phase_at(std::chrono::steady_clock::time_point when) const {
    double elapsed = std::chrono::duration<double>(when - pacing_.start).count();
    const double interval = std::chrono::duration<double>(pacing_.interval).count();
    const double step = std::chrono::duration<double>(pacing_.step).count();
    if (interval <= 0.0 || step <= 0.0) return 1.0f;
    // A picture always sits exactly on the slot nearest to the moment it is drawn: steady motion moves in whole shares
    // of a flip's move, also when a busy GE made it late (it is shown at its slot instead of being dropped).
    elapsed = std::round(elapsed / step) * step;
    return static_cast<float>(std::clamp((elapsed + step) / interval, 0.0, 1.0));
}

void GlBackend::show_final() {
    pacing_.active = false;
    show(flip_.final_texture, flip_.uv_max[0], flip_.uv_max[1]);
}

void GlBackend::show_between(float phase) {
    RenderTarget shown;
    shown.address = flip_.address;
    shown.width = flip_.width;
    shown.height = flip_.height;
    shown.stride = flip_.stride;
    shown.format = flip_.format;
    render_between(shown, *flip_.now, flip_.before, slots_[flip_.before_slot], phase);
    show(scratch_.color, flip_.uv_max[0], flip_.uv_max[1]);
}

// Called by the GE worker when the next picture is due (and it has no display list to run). Draws the picture for
// this moment, however late it is, and asks to be called again one step later; the flip's own picture closes it.
void GlBackend::tick() {
    if (!pacing_.active) {
        redraw_only_ = true; // nothing new to show (the menu is up and no picture came): draw the last one again
        show(last_texture_, last_uv_[0], last_uv_[1]);
        redraw_only_ = false;
        have_state_ = false;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const float phase = phase_at(now);
    if (phase >= 0.999f) {
        show_final();
        return;
    }
    show_between(phase);
    // The next one at the next slot (anchored to the flip, so waking up a little late does not add up), at once if that
    // slot has already passed, and never after the moment the flip's own picture is due.
    const auto final_due = pacing_.start + pacing_.interval - pacing_.step;
    const double slots = std::chrono::duration<double>(now - pacing_.start).count() /
                         std::chrono::duration<double>(pacing_.step).count();
    const std::chrono::steady_clock::time_point next_slot =
        pacing_.start + pacing_.step * static_cast<std::chrono::steady_clock::rep>(std::floor(slots + 0.5) + 1.0);
    pacing_.next_due = std::min(std::max(next_slot, now), final_due);
}

void GlBackend::flush_pictures() {
    if (pacing_.active) show_final(); // the next flip came before this one was fully shown
}

// Schedules the in-between pictures, the first one due at once, ending with the flip's own picture. Returns false
// (nothing scheduled) when this flip cannot be interpolated; the caller then shows it as usual. Either way the picture
// is kept for the next flip.
// Matches the draws of this flip with the previous flip's (the second design) and, with
// MHF_INTERP_V2_LOG=1, reports once a second how the matching went: draws, matched, repaired instances, rejected
// pairs, the camera's turn and move, and the cuts.
void GlBackend::match_v2(std::uint32_t shown) {
    interpolation::mark_eligible(recording_, shown);
    v2_ready_ = false;
    v2_prepared_ = false;
    static const bool log = std::getenv("MHF_INTERP_V2_LOG") != nullptr;
    struct Totals {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        unsigned flips = 0, cuts = 0, continued = 0;
        std::uint64_t eligible = 0, matched = 0, repaired = 0, rejected = 0, crowded = 0, objects = 0, votes = 0, voters = 0;
        float sum_angle = 0;
        float max_angle = 0, max_distance = 0;
        double us = 0;
        std::map<std::string, unsigned> why;
    };
    static Totals totals;
    if (!summaries_before_.empty()) {
        const auto t0 = std::chrono::steady_clock::now();
        const interpolation::Matching &m = matcher_.match(summaries_before_, recording_, cut_thresholds_);
        if (v2_enabled_) {
            v2_older_of_.assign(recording_.size(), -1);
            for (std::size_t i = 0; i < m.newer_of.size(); ++i) {
                if (m.newer_of[i] >= 0) v2_older_of_[static_cast<std::size_t>(m.newer_of[i])] = static_cast<std::int32_t>(i);
            }
            v2_camera_ = m.camera_found ? interpolation::rigid_motion(m.camera) : interpolation::RigidMotion{};
            v2_ready_ = m.cut == nullptr;
        }
        if (log) {
            totals.us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            ++totals.flips;
            totals.eligible += m.eligible_newer;
            totals.matched += m.matched;
            totals.repaired += m.repaired;
            totals.rejected += m.rejected;
            totals.crowded += m.too_many_instances;
            totals.objects += m.objects;
            totals.votes += m.camera_votes;
            totals.voters += m.camera_voters;
            totals.sum_angle += m.camera_angle_degrees;
            if (m.continued) ++totals.continued;
            if (m.cut != nullptr) {
                ++totals.cuts;
                ++totals.why[m.cut];
            }
            if (m.camera_found) {
                totals.max_angle = std::max(totals.max_angle, m.camera_angle_degrees);
                totals.max_distance = std::max(totals.max_distance, m.camera_distance);
            }
        }
    } else {
        matcher_.forget_motion();
    }
    if (v2_enabled_) {
        v2_older_ = std::move(summaries_before_);
        viewports_now_ = std::move(recording_viewports_);
        recording_viewports_.clear();
    }
    summaries_before_ = std::move(recording_);
    recording_.clear();
    if (!log || std::chrono::steady_clock::now() - totals.start < std::chrono::seconds(1) || totals.flips == 0) return;
    std::string why;
    for (const auto &[reason, count] : totals.why) why += " " + reason + " x" + std::to_string(count);
    std::fprintf(stderr, "[interp2] %u flips: %.0f eligible draws, %.1f%% matched, %llu instances repaired (%llu meshes over the cap), %llu rejected, "
                         "%.0f objects, camera votes %.0f%%, turn %.2f deg on average and up to %.1f deg, up to %.1f units, "
                         "%u continued, %u cuts%s, matching %.0f us a flip\n",
                 totals.flips, static_cast<double>(totals.eligible) / totals.flips,
                 totals.eligible != 0 ? 100.0 * static_cast<double>(totals.matched) / static_cast<double>(totals.eligible) : 0.0,
                 static_cast<unsigned long long>(totals.repaired), static_cast<unsigned long long>(totals.crowded),
                 static_cast<unsigned long long>(totals.rejected), static_cast<double>(totals.objects) / totals.flips,
                 totals.voters != 0 ? 100.0 * static_cast<double>(totals.votes) / static_cast<double>(totals.voters) : 0.0,
                 static_cast<double>(totals.sum_angle) / totals.flips, static_cast<double>(totals.max_angle),
                 static_cast<double>(totals.max_distance), totals.continued, totals.cuts,
                 why.c_str(), totals.us / totals.flips);
    totals = Totals{};
}

bool GlBackend::present_interpolated(RenderTarget &shown, const float uv_max[2]) {
    const auto now = std::chrono::steady_clock::now();
    const auto interval = now - last_flip_;
    last_flip_ = now;

    const GLsizei w = static_cast<GLsizei>(shown.width) * scale_, h = static_cast<GLsizei>(shown.height) * scale_;
    ensure_slot(slots_[0], w, h);
    ensure_slot(slots_[1], w, h);

    match_v2(shown.address);

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
            if (is_effect(e.setup.key)) unpair_jumping_particles(e.origins);
        }
        flip_.address = shown.address;
        flip_.width = shown.width;
        flip_.height = shown.height;
        flip_.stride = shown.stride;
        flip_.format = shown.format;
        flip_.before_slot = 1 - fill_slot_;
        flip_.final_texture = slots_[fill_slot_].texture;
        flip_.shadow = shadow_pass_;
        flip_.shadow.valid = shadow_pass_.valid && shadow_done_frame_ == frame_ && shadow_pass_.address == shown.address;
        flip_.uv_max[0] = uv_max[0];
        flip_.uv_max[1] = uv_max[1];
        pacing_.active = true;
        pacing_.start = now;
        pacing_.interval = interval;
        pacing_.step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval / factor_);
        pacing_.next_due = now; // the first in-between picture goes out at once
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
                     "menus %llu; pictures on screen %.0f per second\n",
                     factor_, static_cast<unsigned long long>(stats_.interpolated),
                     static_cast<unsigned long long>(stats_.skipped_first), static_cast<unsigned long long>(stats_.skipped_copy),
                     static_cast<unsigned long long>(stats_.skipped_timing), static_cast<unsigned long long>(stats_.skipped_shape),
                     static_cast<unsigned long long>(stats_.skipped_ui),
                     log_time_ == std::chrono::steady_clock::time_point{}
                         ? 0.0
                         : static_cast<double>(shown_ - log_shown_) /
                               std::chrono::duration<double>(now - log_time_).count());
        log_shown_ = shown_;
        log_time_ = now;
        if (cost_.shows > 0) {
            const double s = static_cast<double>(cost_.shows);
            std::fprintf(stderr, "[interp] each picture put on the window: %.2f ms in total, %.2f ms of it in the buffer swap\n",
                         cost_.show_us / s / 1000.0, cost_.swap_us / s / 1000.0);
        }
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

// Draws `source` small and blurs it a few times, wider each time, into blur_texture_[0].
void GlBackend::build_blur(GLuint source, float uv_max_x, float uv_max_y) {
    if (blur_fbo_[0] == 0) {
        gl_.GenTextures(2, blur_texture_);
        gl_.GenFramebuffers(2, blur_fbo_);
        for (int i = 0; i < 2; ++i) {
            gl_.BindTexture(GL_TEXTURE_2D, blur_texture_[i]);
            gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kBlurW, kBlurH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            set_texture_params(true, true, true);
            gl_.BindFramebuffer(GL_FRAMEBUFFER, blur_fbo_[i]);
            gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blur_texture_[i], 0);
        }
    }
    gl_.Disable(GL_SCISSOR_TEST);
    gl_.Disable(GL_DEPTH_TEST);
    gl_.Disable(GL_BLEND);
    gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl_.Viewport(0, 0, kBlurW, kBlurH);
    gl_.BindVertexArray(empty_vao_);
    gl_.ActiveTexture(GL_TEXTURE0);

    // The picture itself, shrunk: with the present shader (which flips), so row 0 of the target is the picture's bottom.
    gl_.BindFramebuffer(GL_FRAMEBUFFER, blur_fbo_[0]);
    gl_.UseProgram(present_program_);
    gl_.Uniform1i(present_tex_, 0);
    gl_.Uniform2f(present_uv_max_, uv_max_x, uv_max_y);
    gl_.BindTexture(GL_TEXTURE_2D, source);
    set_texture_params(true, true, true);
    gl_.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Blur: horizontal then vertical, three times, each wider than the last. These passes do not flip.
    gl_.UseProgram(blur_program_);
    gl_.Uniform1i(blur_tex_, 0);
    gl_.Uniform2f(blur_uv_off_, 0.0f, 0.0f);
    gl_.Uniform2f(blur_uv_scale_, 1.0f, 1.0f);
    gl_.Uniform1f(blur_dim_, 1.0f);
    const float widths[3] = {1.0f, 2.0f, 4.0f};
    for (const float width : widths) {
        const float steps[2][2] = {{width / kBlurW, 0.0f}, {0.0f, width / kBlurH}};
        for (const auto &step : steps) {
            gl_.BindFramebuffer(GL_FRAMEBUFFER, blur_fbo_[1]);
            gl_.BindTexture(GL_TEXTURE_2D, blur_texture_[0]);
            gl_.Uniform2f(blur_dir_, step[0], step[1]);
            gl_.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            std::swap(blur_texture_[0], blur_texture_[1]); // the result is blur_texture_[0] again
            std::swap(blur_fbo_[0], blur_fbo_[1]);
        }
    }
    blur_source_ = source;
    blur_ready_ = true;
    have_state_ = false;
}

// Fills the window with the blurred picture, cropped (not stretched) to the window's shape, and darkened.
void GlBackend::draw_blurred_background(int window_w, int window_h) {
    gl_.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_.Viewport(0, 0, window_w, window_h);
    gl_.Disable(GL_BLEND);
    gl_.Disable(GL_DEPTH_TEST);
    gl_.UseProgram(blur_program_);
    gl_.Uniform1i(blur_tex_, 0);
    const float picture = 480.0f / 272.0f, window = static_cast<float>(window_w) / static_cast<float>(window_h);
    float off_x = 0.0f, off_y = 0.0f, scale_x = 1.0f, scale_y = 1.0f;
    if (window > picture) { // wider than the picture: crop its top and bottom
        scale_y = picture / window;
        off_y = (1.0f - scale_y) * 0.5f;
    } else {                // taller: crop its sides
        scale_x = window / picture;
        off_x = (1.0f - scale_x) * 0.5f;
    }
    gl_.Uniform2f(blur_uv_off_, off_x, off_y);
    gl_.Uniform2f(blur_uv_scale_, scale_x, scale_y);
    gl_.Uniform2f(blur_dir_, 0.0f, 0.0f);
    gl_.Uniform1f(blur_dim_, 0.55f);
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_2D, blur_texture_[0]);
    set_texture_params(true, true, true);
    gl_.BindVertexArray(empty_vao_);
    gl_.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// The player changed the internal resolution: every render target is drawn again at the new size (what was on it is
// scaled, so the picture does not flash), and what was made for the old size is dropped.
void GlBackend::apply_live_scale() {
    const int wanted = std::clamp(live_->scale.exchange(0), 0, 10);
    if (wanted == 0 || wanted == scale_) return;
    have_state_ = false;
    const int old_scale = scale_;
    for (auto &entry : targets_) {
        RenderTarget &rt = entry.second;
        const GLuint old_fbo = rt.fbo, old_color = rt.color, old_depth = rt.depth;
        const GLsizei old_w = static_cast<GLsizei>(rt.width) * old_scale, old_h = static_cast<GLsizei>(rt.height) * old_scale;
        rt.fbo = 0; // allocate() must not copy from the old one at the old size
        scale_ = wanted;
        allocate(rt, rt.height);
        scale_ = old_scale;
        const GLsizei new_w = static_cast<GLsizei>(rt.width) * wanted, new_h = static_cast<GLsizei>(rt.height) * wanted;
        gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, old_fbo);
        gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, rt.fbo);
        gl_.Disable(GL_SCISSOR_TEST);
        gl_.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl_.BlitFramebuffer(0, 0, old_w, old_h, 0, 0, new_w, new_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        gl_.DeleteFramebuffers(1, &old_fbo);
        gl_.DeleteTextures(1, &old_color);
        gl_.DeleteTextures(1, &old_depth);
    }
    scale_ = wanted;
    for (Slot &slot : slots_) {
        if (slot.fbo != 0) {
            gl_.DeleteFramebuffers(1, &slot.fbo);
            gl_.DeleteTextures(1, &slot.texture);
        }
        slot = Slot{};
    }
    if (scratch_.fbo != 0) {
        gl_.DeleteFramebuffers(1, &scratch_.fbo);
        gl_.DeleteTextures(1, &scratch_.color);
        gl_.DeleteTextures(1, &scratch_.depth);
    }
    scratch_ = RenderTarget{};
    previous_valid_ = false;
    last_texture_ = 0;
    blur_ready_ = false;
    std::fprintf(stderr, "[gl] internal resolution now %dx%d\n", 480 * scale_, 272 * scale_);
}

// The player changed the frame rate: from the next picture on, the game's flips are followed by the new number of
// in-between pictures (or by none).
void GlBackend::apply_live_fps() {
    const int fps = live_->fps.exchange(0);
    if (fps <= 0) return;
    const int factor = std::clamp((fps + 15) / 30, 1, 6);
    if (factor == factor_) return;
    factor_ = factor;
    interpolate_ = factor > 1;
    previous_valid_ = false;
    previous_.clear();
    std::fprintf(stderr, "[gl] frame rate now %d fps\n", factor_ * 30);
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
                                              bool smooth_textures, OverlayImage *overlay, int target_fps,
                                              LiveSettings *live) {
    if (const char *v = std::getenv("MHF_FPS")) target_fps = std::atoi(v); // testing aid
    // The game flips at 30 fps: the output rate is a whole multiple of it, from 1x (as the game) to 6x (180 fps).
    const int factor = std::clamp((target_fps + 15) / 30, 1, 6);
    auto backend = std::make_unique<GlBackend>(window, scale, memory, smooth_textures, overlay, factor, live);
    if (!backend->init()) return nullptr;
    return backend;
}

} // namespace mhf::psp
