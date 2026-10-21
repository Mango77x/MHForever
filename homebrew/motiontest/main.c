// motiontest: a few shapes that move a fixed distance every frame, at 30 fps like the original game's main loop
// (two vblanks per frame). Used to check frame interpolation (docs/05-runtime-hle.md): the dumped frames show where
// the shapes are. Our own code; GPL-3.0-or-later like the rest of the repository.
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <stdio.h>

PSP_MODULE_INFO("motiontest", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define FRAMES 40
#define STEP 8 /* pixels per game frame */

static unsigned int __attribute__((aligned(16))) list[262144];

struct ColorVertex { unsigned int color; short x, y, z; };

static void sprite(unsigned color, int x0, int y0, int x1, int y1) {
    struct ColorVertex *s = sceGuGetMemory(2 * sizeof(struct ColorVertex));
    s[0] = (struct ColorVertex){color, (short)x0, (short)y0, 0};
    s[1] = (struct ColorVertex){color, (short)x1, (short)y1, 0};
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, s);
}

static void triangle(unsigned color, int x, int y) {
    struct ColorVertex *t = sceGuGetMemory(3 * sizeof(struct ColorVertex));
    t[0] = (struct ColorVertex){color, (short)x, (short)y, 0};
    t[1] = (struct ColorVertex){color, (short)(x + 40), (short)y, 0};
    t[2] = (struct ColorVertex){color, (short)x, (short)(y + 40), 0};
    sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 3, 0, t);
}

int main(void) {
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

    for (int frame = 0; frame < FRAMES; ++frame) {
        sceGuStart(GU_DIRECT, list);
        sceGuClearColor(0xFF202020);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        sprite(0xFF0000FF, 20 + frame * STEP, 40, 60 + frame * STEP, 80);        // red: moves right
        sprite(0xFF00FF00, 400 - frame * STEP, 120, 440 - frame * STEP, 160);    // green: moves left
        triangle(0xFFFF8000, 40 + frame * STEP / 2, 180);                         // blue-ish triangle: half speed
        sprite(0xFFFFFFFF, 10, 250, 110, 262);                                     // HUD bar: never moves
        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceDisplayWaitVblankStart(); // 30 fps
        sceGuSwapBuffers();
    }
    printf("motiontest: done\n");
    sceKernelExitGame();
    return 0;
}
