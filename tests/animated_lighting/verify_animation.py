"""Render a public, synthetic two-bone fixture in an EGL/OpenGL context."""
import ctypes as c
import os
from math import sqrt

SIZE = 96
GL_VERTEX_SHADER = 0x8B31
GL_FRAGMENT_SHADER = 0x8B30
GL_COMPILE_STATUS = 0x8B81
GL_LINK_STATUS = 0x8B82
GL_RGBA = 0x1908
GL_UNSIGNED_BYTE = 0x1401

VERTEX = """#version 330 core
uniform vec2 rotations;
out vec3 worldNormal;
out float vertexLight;
const vec2 screen[3] = vec2[3](vec2(16.5, 16.5), vec2(80.5, 16.5), vec2(48.5, 64.5));
const vec3 normals[3] = vec3[3](vec3(0,0,1), vec3(1,0,0), vec3(0,1,0));
mat3 rotateY(float cosine, float sine) {
    return mat3(cosine,0,-sine, 0,1,0, sine,0,cosine);
}
void main() {
    mat3 a = rotateY(rotations.x, rotations.y);
    mat3 b = rotateY(rotations.x, -rotations.y);
    vec2 ndc = screen[gl_VertexID] / 48.0 - 1.0;
    // Keep the pixel centroid fixed while independently testing animated normals.
    gl_Position = vec4(ndc, 0.0, 1.0);
    vec3 normal = normals[gl_VertexID];
    worldNormal = normalize(0.75 * (a * normal) + 0.25 * (b * normal));
    vertexLight = max(worldNormal.z, 0.0);
}
"""
FRAGMENT = """#version 330 core
uniform int fragmentLighting;
in vec3 worldNormal;
in float vertexLight;
out vec4 color;
void main() {
    float light = fragmentLighting == 1 ? max(normalize(worldNormal).z, 0.0) : vertexLight;
    color = vec4(vec3(light), 1.0);
}
"""


def bind(library, name, result, *params):
    function = getattr(library, name)
    function.restype = result
    function.argtypes = list(params)
    return function


class Context:
    def __enter__(self):
        os.environ.setdefault("EGL_PLATFORM", "surfaceless")
        egl = c.CDLL("libEGL.so.1")
        handle = c.c_void_p
        int_pointer = c.POINTER(c.c_int)
        self.display = bind(egl, "eglGetDisplay", handle, handle)(None)
        self.terminate = bind(egl, "eglTerminate", c.c_uint, handle)
        self.destroy_context = bind(egl, "eglDestroyContext", c.c_uint, handle, handle)
        self.destroy_surface = bind(egl, "eglDestroySurface", c.c_uint, handle, handle)
        self.make_current = bind(egl, "eglMakeCurrent", c.c_uint, handle, handle, handle, handle)
        self.context = self.surface = None
        self.error = bind(egl, "eglGetError", c.c_uint)
        self.require(bind(egl, "eglInitialize", c.c_uint, handle, int_pointer, int_pointer)(
            self.display, None, None
        ), "initialize EGL")
        try:
            self.require(bind(egl, "eglBindAPI", c.c_uint, c.c_uint)(0x30A2), "bind OpenGL")
            # Pbuffer, OpenGL, RGBA8.
            config_attributes = (c.c_int * 13)(
                0x3033, 1, 0x3040, 8, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038
            )
            config, count = handle(), c.c_int()
            choose = bind(egl, "eglChooseConfig", c.c_uint, handle, int_pointer,
                          c.POINTER(handle), c.c_int, int_pointer)
            self.require(choose(self.display, config_attributes, c.byref(config), 1, c.byref(count))
                         and count.value, "choose an RGBA8 config")
            dimensions = (c.c_int * 5)(0x3057, SIZE, 0x3056, SIZE, 0x3038)
            surface = bind(egl, "eglCreatePbufferSurface", handle, handle, handle, int_pointer)
            self.surface = surface(self.display, config, dimensions)
            self.require(self.surface, "create the pbuffer")
            attributes = (c.c_int * 7)(0x3098, 3, 0x30FB, 3, 0x30FD, 1, 0x3038)
            create = bind(egl, "eglCreateContext", handle, handle, handle, handle, int_pointer)
            self.context = create(self.display, config, None, attributes)
            self.require(self.context, "create an OpenGL 3.3 core context")
            self.require(self.make_current(self.display, self.surface, self.surface, self.context),
                         "make the OpenGL context current")
            return c.CDLL("libGL.so.1")
        except Exception:
            self.__exit__()
            raise

    def require(self, ok, action):
        if not ok:
            raise RuntimeError(f"Failed to {action} (EGL 0x{self.error():04x})")

    def __exit__(self, *_):
        self.make_current(self.display, None, None, None)
        if self.context:
            self.destroy_context(self.display, self.context)
        if self.surface:
            self.destroy_surface(self.display, self.surface)
        self.terminate(self.display)


def check_object(gl, kind, object_id, flag):
    status = c.c_int()
    bind(gl, f"glGet{kind}iv", None, c.c_uint, c.c_uint, c.POINTER(c.c_int))(
        object_id, flag, c.byref(status)
    )
    if status.value:
        return
    log = c.create_string_buffer(4096)
    bind(gl, f"glGet{kind}InfoLog", None, c.c_uint, c.c_int, c.POINTER(c.c_int), c.c_void_p)(
        object_id, len(log), None, log
    )
    raise RuntimeError(log.value.decode(errors="replace"))


def build_program(gl):
    create_shader = bind(gl, "glCreateShader", c.c_uint, c.c_uint)
    set_source = bind(gl, "glShaderSource", None, c.c_uint, c.c_int,
                      c.POINTER(c.c_char_p), c.POINTER(c.c_int))
    compile_shader = bind(gl, "glCompileShader", None, c.c_uint)
    program = bind(gl, "glCreateProgram", c.c_uint)()
    attach = bind(gl, "glAttachShader", None, c.c_uint, c.c_uint)
    delete = bind(gl, "glDeleteShader", None, c.c_uint)
    for kind, source in ((GL_VERTEX_SHADER, VERTEX), (GL_FRAGMENT_SHADER, FRAGMENT)):
        shader = create_shader(kind)
        set_source(shader, 1, c.byref(c.c_char_p(source.encode())), None)
        compile_shader(shader)
        check_object(gl, "Shader", shader, GL_COMPILE_STATUS)
        attach(program, shader)
        delete(shader)
    bind(gl, "glLinkProgram", None, c.c_uint)(program)
    check_object(gl, "Program", program, GL_LINK_STATUS)
    bind(gl, "glUseProgram", None, c.c_uint)(program)
    return program


def verify(gl):
    renderer = bind(gl, "glGetString", c.c_char_p, c.c_uint)(0x1F01).decode()
    print(f"Renderer: {renderer}", flush=True)
    program = build_program(gl)
    vao = c.c_uint()
    bind(gl, "glGenVertexArrays", None, c.c_int, c.POINTER(c.c_uint))(1, c.byref(vao))
    bind(gl, "glBindVertexArray", None, c.c_uint)(vao)
    bind(gl, "glViewport", None, c.c_int, c.c_int, c.c_int, c.c_int)(0, 0, SIZE, SIZE)
    bind(gl, "glDisable", None, c.c_uint)(0x0BD0)  # GL_DITHER
    clear = bind(gl, "glClear", None, c.c_uint)
    clear_color = bind(gl, "glClearColor", None, c.c_float, c.c_float, c.c_float, c.c_float)
    uniform = bind(gl, "glGetUniformLocation", c.c_int, c.c_uint, c.c_char_p)
    set_pose = bind(gl, "glUniform2f", None, c.c_int, c.c_float, c.c_float)
    set_mode = bind(gl, "glUniform1i", None, c.c_int, c.c_int)
    draw = bind(gl, "glDrawArrays", None, c.c_uint, c.c_int, c.c_int)
    read = bind(gl, "glReadPixels", None, c.c_int, c.c_int, c.c_int, c.c_int,
                c.c_uint, c.c_uint, c.c_void_p)
    error = bind(gl, "glGetError", c.c_uint)
    rotation = uniform(program, b"rotations")
    mode = uniform(program, b"fragmentLighting")
    if min(rotation, mode) < 0:
        raise RuntimeError("An animation uniform was not active")

    # Centroid (48.5, 32.5); independent RGBA8 oracles from barycentric weights 1/3.
    # At 60 degrees the normalised blend has cos = 2/sqrt(7), sin = sqrt(3)/sqrt(7).
    poses = (
        ("0 deg", 1.0, 0.0, (85, 147)),
        ("+60 deg", 0.5, sqrt(3) / 2, (64, 15)),
        ("-60 deg", 0.5, -sqrt(3) / 2, (120, 208)),
        ("+60 deg", 0.5, sqrt(3) / 2, (64, 15)),
        ("0 deg", 1.0, 0.0, (85, 147)),
    )
    checks = 0
    for enabled in (0, 1):
        set_mode(mode, enabled)
        for label, cosine, sine, expected in poses:
            set_pose(rotation, cosine, sine)
            clear_color(0.0, 0.0, 0.0, 0.0)
            clear(0x4000)
            draw(4, 0, 3)
            pixel = (c.c_ubyte * 4)()
            read(48, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel)
            actual = tuple(pixel)
            status = error()
            if status or any(abs(channel - expected[enabled]) > 2 for channel in actual[:3]) or actual[3] != 255:
                raise RuntimeError(f"mode {enabled}, pose {label}: got {actual}, "
                                   f"expected RGB {expected[enabled]} and alpha 255 (GL error 0x{status:04x})")
            print(f"{'fragment' if enabled else 'vertex':8} {label:8} {actual}")
            checks += 1
    print(f"PASS: {checks} framebuffer checks")


if __name__ == "__main__":
    with Context() as gl:
        verify(gl)
