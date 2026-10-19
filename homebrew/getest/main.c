// getest: draws known shapes with the GE and reads the framebuffer back (docs/05-runtime-hle.md §6).
// Our own code; GPL-3.0-or-later like the rest of the repository.
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>

PSP_MODULE_INFO("getest", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

static unsigned int __attribute__((aligned(16))) list[262144];
static unsigned int __attribute__((aligned(16))) texture[4 * 4];
static int passed, failed;

#define VRAM ((volatile unsigned int *)0x44000000)
#define PIXEL(x, y) VRAM[(y) * 512 + (x)]

static void check_pixel(const char *name, int x, int y, unsigned expected, int tolerance) {
    // Alpha holds stencil on the PSP and depends on stencil settings, so only RGB is compared.
    unsigned got = PIXEL(x, y) & 0x00FFFFFFu;
    expected &= 0x00FFFFFFu;
    int ok = 1;
    for (int i = 0; i < 24; i += 8) {
        int a = (got >> i) & 0xFF, b = (expected >> i) & 0xFF;
        if (abs(a - b) > tolerance) ok = 0;
    }
    if (ok) ++passed;
    else { ++failed; printf("FAIL %s at (%d,%d): got %08x expected %08x\n", name, x, y, got, expected); }
}

struct ColorVertex { unsigned int color; short x, y, z; };
struct TexVertex { short u, v; short x, y, z; };
struct Vertex3D { unsigned int color; float x, y, z; };

int main(void) {
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            texture[y * 4 + x] = ((x ^ y) & 1) ? 0xFFFFFFFFu : 0xFF00FF00u; // white / green checker

    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, 512);
    sceGuDispBuffer(480, 272, (void *)0x88000, 512);
    sceGuDepthBuffer((void *)0x110000, 512);
    sceGuOffset(2048 - 240, 2048 - 136);
    sceGuViewport(2048, 2048, 480, 272);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, 480, 272);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuDisplay(GU_TRUE);

    sceGuStart(GU_DIRECT, list);
    sceGuClearColor(0xFF402010);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

    // Through-mode sprite.
    struct ColorVertex *s = sceGuGetMemory(2 * sizeof(struct ColorVertex));
    s[0] = (struct ColorVertex){0xFF0000FF, 10, 10, 0};
    s[1] = (struct ColorVertex){0xFF0000FF, 50, 50, 0};
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, s);

    // Through-mode triangle.
    struct ColorVertex *t = sceGuGetMemory(3 * sizeof(struct ColorVertex));
    t[0] = (struct ColorVertex){0xFF00FF00, 60, 10, 0};
    t[1] = (struct ColorVertex){0xFF00FF00, 120, 10, 0};
    t[2] = (struct ColorVertex){0xFF00FF00, 60, 70, 0};
    sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 3, 0, t);

    // Textured sprite: 4x4 checker stretched to 64x64.
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, 4, 4, 4, texture);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    struct TexVertex *tv = sceGuGetMemory(2 * sizeof(struct TexVertex));
    tv[0] = (struct TexVertex){0, 0, 140, 10, 0};
    tv[1] = (struct TexVertex){4, 4, 204, 74, 0};
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, tv);
    sceGuDisable(GU_TEXTURE_2D);

    // Alpha blending: half-transparent white over the red sprite.
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    struct ColorVertex *b = sceGuGetMemory(2 * sizeof(struct ColorVertex));
    b[0] = (struct ColorVertex){0x80FFFFFF, 30, 30, 0};
    b[1] = (struct ColorVertex){0x80FFFFFF, 50, 50, 0};
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, b);
    sceGuDisable(GU_BLEND);

    // A transformed, perspective-projected triangle in front of the camera, centered on screen.
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(75.0f, 16.0f / 9.0f, 0.5f, 1000.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    struct Vertex3D *v = sceGuGetMemory(3 * sizeof(struct Vertex3D));
    v[0] = (struct Vertex3D){0xFFFF0000, -1.0f, -1.0f, -3.0f};
    v[1] = (struct Vertex3D){0xFFFF0000, 1.0f, -1.0f, -3.0f};
    v[2] = (struct Vertex3D){0xFFFF0000, 0.0f, 1.0f, -3.0f};
    sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 3, 0, v);

    // Block transfer: copy the red sprite area to (400,200).
    sceGuCopyImage(GU_PSM_8888, 10, 10, 16, 16, 512, (void *)0x44000000, 400, 200, 512, (void *)0x44000000);
    sceGuTexSync();

    sceGuFinish();
    sceGuSync(0, 0);

    check_pixel("clear color", 300, 150, 0xFF402010, 0);
    check_pixel("sprite", 20, 20, 0xFF0000FF, 0);
    check_pixel("sprite edge excluded", 50, 20, 0xFF402010, 0);
    check_pixel("triangle inside", 70, 20, 0xFF00FF00, 0);
    check_pixel("triangle outside", 115, 60, 0xFF402010, 0);
    check_pixel("texture texel 0,0", 145, 15, 0xFF00FF00, 0);
    check_pixel("texture texel 1,0", 161, 15, 0xFFFFFFFF, 0);
    check_pixel("texture texel 1,1", 161, 31, 0xFF00FF00, 0);
    check_pixel("alpha blend", 40, 40, 0xFF8080FF, 2);
    check_pixel("3d triangle center", 240, 140, 0xFFFF0000, 0);
    check_pixel("3d triangle outside", 240, 30, 0xFF402010, 0);
    check_pixel("block transfer", 405, 205, 0xFF0000FF, 0);

    printf("getest: %d passed, %d failed\n", passed, failed);
    sceKernelExitGame();
    return 0;
}
