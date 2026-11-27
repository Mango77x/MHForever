// Compiles and links all three assembled SMAA GLSL 330 Core shader pairs in an OpenGL 3.3 Core context.
// Exits with code 77 (CTest SKIP_RETURN_CODE) only when the host cannot create an OpenGL 3.3 Core context.

#include "smaa_shaders.hpp"

#include "check.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>
#include <string>

namespace {

constexpr int kSkipCode = 77;

struct GlShaderApi {
    PFNGLCREATESHADERPROC CreateShader{};
    PFNGLDELETESHADERPROC DeleteShader{};
    PFNGLSHADERSOURCEPROC ShaderSource{};
    PFNGLCOMPILESHADERPROC CompileShader{};
    PFNGLGETSHADERIVPROC GetShaderiv{};
    PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog{};
    PFNGLCREATEPROGRAMPROC CreateProgram{};
    PFNGLDELETEPROGRAMPROC DeleteProgram{};
    PFNGLATTACHSHADERPROC AttachShader{};
    PFNGLLINKPROGRAMPROC LinkProgram{};
    PFNGLGETPROGRAMIVPROC GetProgramiv{};
    PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog{};

    bool load() {
        auto get = [](const char *name) { return SDL_GL_GetProcAddress(name); };
        CreateShader = reinterpret_cast<PFNGLCREATESHADERPROC>(get("glCreateShader"));
        DeleteShader = reinterpret_cast<PFNGLDELETESHADERPROC>(get("glDeleteShader"));
        ShaderSource = reinterpret_cast<PFNGLSHADERSOURCEPROC>(get("glShaderSource"));
        CompileShader = reinterpret_cast<PFNGLCOMPILESHADERPROC>(get("glCompileShader"));
        GetShaderiv = reinterpret_cast<PFNGLGETSHADERIVPROC>(get("glGetShaderiv"));
        GetShaderInfoLog = reinterpret_cast<PFNGLGETSHADERINFOLOGPROC>(get("glGetShaderInfoLog"));
        CreateProgram = reinterpret_cast<PFNGLCREATEPROGRAMPROC>(get("glCreateProgram"));
        DeleteProgram = reinterpret_cast<PFNGLDELETEPROGRAMPROC>(get("glDeleteProgram"));
        AttachShader = reinterpret_cast<PFNGLATTACHSHADERPROC>(get("glAttachShader"));
        LinkProgram = reinterpret_cast<PFNGLLINKPROGRAMPROC>(get("glLinkProgram"));
        GetProgramiv = reinterpret_cast<PFNGLGETPROGRAMIVPROC>(get("glGetProgramiv"));
        GetProgramInfoLog = reinterpret_cast<PFNGLGETPROGRAMINFOLOGPROC>(get("glGetProgramInfoLog"));
        return CreateShader && DeleteShader && ShaderSource && CompileShader && GetShaderiv && GetShaderInfoLog &&
               CreateProgram && DeleteProgram && AttachShader && LinkProgram && GetProgramiv && GetProgramInfoLog;
    }
};

bool compile_and_link(const GlShaderApi &gl, const char *label, const std::string &vs_src, const std::string &fs_src) {
    auto compile = [&](GLenum stage, const char *stage_name, const std::string &src) -> GLuint {
        const GLuint shader = gl.CreateShader(stage);
        const char *ptr = src.c_str();
        gl.ShaderSource(shader, 1, &ptr, nullptr);
        gl.CompileShader(shader);
        GLint ok = 0;
        gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096]{};
            gl.GetShaderInfoLog(shader, sizeof log, nullptr, log);
            std::fprintf(stderr, "[psp_smaa_gl] %s %s compile error:\n%s\n", label, stage_name, log);
            gl.DeleteShader(shader);
            return 0;
        }
        return shader;
    };

    const GLuint vs = compile(GL_VERTEX_SHADER, "vertex", vs_src);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, "fragment", fs_src);
    if (vs == 0 || fs == 0) {
        if (vs != 0) gl.DeleteShader(vs);
        if (fs != 0) gl.DeleteShader(fs);
        return false;
    }
    const GLuint prog = gl.CreateProgram();
    gl.AttachShader(prog, vs);
    gl.AttachShader(prog, fs);
    gl.LinkProgram(prog);
    GLint ok = 0;
    gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096]{};
        gl.GetProgramInfoLog(prog, sizeof log, nullptr, log);
        std::fprintf(stderr, "[psp_smaa_gl] %s link error:\n%s\n", label, log);
    }
    gl.DeleteProgram(prog);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    return ok != 0;
}

} // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[psp_smaa_gl] SDL_Init(VIDEO) unavailable (%s); skipping\n", SDL_GetError());
        return kSkipCode;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *window = SDL_CreateWindow("psp_smaa_gl", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (window == nullptr) {
        std::fprintf(stderr, "[psp_smaa_gl] OpenGL window unavailable (%s); skipping\n", SDL_GetError());
        SDL_Quit();
        return kSkipCode;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (ctx == nullptr) {
        std::fprintf(stderr, "[psp_smaa_gl] OpenGL 3.3 Core context unavailable (%s); skipping\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return kSkipCode;
    }
    SDL_GL_MakeCurrent(window, ctx);

    GlShaderApi gl;
    if (!gl.load()) {
        std::fprintf(stderr, "[psp_smaa_gl] OpenGL shader entry points unavailable; skipping\n");
        SDL_GL_DestroyContext(ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return kSkipCode;
    }

    using namespace mhf::psp::smaa;
    MHF_CHECK(compile_and_link(gl, "EdgeDetection", vertex_shader(Pass::EdgeDetection), fragment_shader(Pass::EdgeDetection)));
    MHF_CHECK(compile_and_link(gl, "BlendingWeight", vertex_shader(Pass::BlendingWeight), fragment_shader(Pass::BlendingWeight)));
    MHF_CHECK(compile_and_link(gl, "NeighborhoodBlending", vertex_shader(Pass::NeighborhoodBlending), fragment_shader(Pass::NeighborhoodBlending)));

    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return mhf::test::finish("psp_smaa_gl");
}
