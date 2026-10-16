// vfputest: VFPU semantics checked against hand-computed values (docs/04-recompiler.md §8).
// Our own code; GPL-2.0-only like the rest of the repository.
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("vfputest", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

static int passed, failed;
static float in_a[4] __attribute__((aligned(16))) = {1.0f, 2.0f, 3.0f, 4.0f};
static float in_b[4] __attribute__((aligned(16))) = {0.5f, -1.0f, 2.0f, 0.25f};
static float mat[16] __attribute__((aligned(16))) = {
    1, 0, 0, 0,   // column 0
    0, 2, 0, 0,   // column 1
    0, 0, 3, 0,   // column 2
    10, 20, 30, 1 // column 3 (translation)
};
static float out[16] __attribute__((aligned(16)));

static void near(const char *name, float got, float expected, float tolerance) {
    float d = got - expected;
    if (d < 0) d = -d;
    if (d <= tolerance) ++passed;
    else { ++failed; printf("FAIL %s: got %f expected %f\n", name, (double)got, (double)expected); }
}

#define LOAD_AB "lv.q C000, 0(%0)\n lv.q C010, 0(%1)\n"

int main(void) {
    // Vector arithmetic.
    __asm__ volatile(LOAD_AB "vsub.q C020, C000, C010\n sv.q C020, 0(%2)" : : "r"(in_a), "r"(in_b), "r"(out) : "memory");
    near("vsub.q y", out[1], 3.0f, 0);
    __asm__ volatile(LOAD_AB "vdiv.q C020, C000, C010\n sv.q C020, 0(%2)" : : "r"(in_a), "r"(in_b), "r"(out) : "memory");
    near("vdiv.q w", out[3], 16.0f, 1e-5f);
    __asm__ volatile(LOAD_AB "vmax.q C020, C000, C010\n vmin.q C030, C000, C010\n sv.q C020, 0(%2)\n sv.q C030, 16(%2)"
                     : : "r"(in_a), "r"(in_b), "r"(out) : "memory");
    near("vmax.q y", out[1], 2.0f, 0);
    near("vmin.q y", out[5], -1.0f, 0);
    __asm__ volatile(LOAD_AB "vhdp.q S020, C000, C010\n sv.s S020, 0(%2)" : : "r"(in_a), "r"(in_b), "r"(out) : "memory");
    near("vhdp.q", out[0], 0.5f - 2.0f + 6.0f + 0.25f, 1e-6f); // dot(a.xyz1, b)
    __asm__ volatile(LOAD_AB "vfad.q S020, C000\n vavg.q S021, C000\n sv.s S020, 0(%2)\n sv.s S021, 4(%2)"
                     : : "r"(in_a), "r"(in_b), "r"(out) : "memory");
    near("vfad.q", out[0], 10.0f, 0);
    near("vavg.q", out[1], 2.5f, 0);

    // Unary functions.
    __asm__ volatile("lv.q C000, 0(%0)\n vsqrt.q C020, C000\n vrcp.q C030, C000\n sv.q C020, 0(%1)\n sv.q C030, 16(%1)"
                     : : "r"(in_a), "r"(out) : "memory");
    near("vsqrt.q w", out[3], 2.0f, 1e-5f);
    near("vrcp.q w", out[7], 0.25f, 1e-6f);
    __asm__ volatile("lv.q C000, 0(%0)\n vrsq.q C020, C000\n sv.q C020, 0(%1)" : : "r"(in_a), "r"(out) : "memory");
    near("vrsq.q w", out[3], 0.5f, 1e-5f);
    __asm__ volatile("lv.q C010, 0(%0)\n vsin.s S000, S010\n vcos.s S001, S010\n sv.s S000, 0(%1)\n sv.s S001, 4(%1)"
                     : : "r"(in_b), "r"(out) : "memory");
    // The VFPU's sin/cos take quarter turns: sin(0.5 * pi/2), cos(0.5 * pi/2).
    near("vsin.s", out[0], 0.70710678f, 1e-5f);
    near("vcos.s", out[1], 0.70710678f, 1e-5f);

    // Prefixes: swizzle/negate sources, saturate destination.
    __asm__ volatile("lv.q C000, 0(%0)\n .word 0xDC01001B\n vmov.q C020, C000\n sv.q C020, 0(%1)"
                     : : "r"(in_a), "r"(out) : "memory");
    near("vpfxs swizzle+neg x", out[0], -4.0f, 0);
    near("vpfxs swizzle y", out[1], 3.0f, 0);
    __asm__ volatile("lv.q C010, 0(%0)\n .word 0xDE00044D\n vmov.q C020, C010\n lv.q C030, 0(%0)\n"
                     " sv.q C020, 0(%1)" : : "r"(in_b), "r"(out) : "memory");
    near("vpfxd sat0 x", out[0], 0.5f, 0);
    near("vpfxd sat1 y", out[1], -1.0f, 0);
    near("vpfxd sat0 w", out[3], 0.25f, 0);
    __asm__ volatile(".word 0xDC00F831\n vmov.q C020, C000\n sv.q C020, 0(%0)" : : "r"(out) : "memory");
    near("vpfxs constant 1", out[0], 1.0f, 0);
    near("vpfxs constant 1/2", out[2], 0.5f, 0);
    near("vpfxs constant 3", out[3], 3.0f, 0);

    // Conversions.
    static float halves[4] __attribute__((aligned(16))) = {1.5f, -1.5f, 2.5f, -2.7f};
    static int ints[4] __attribute__((aligned(16)));
    __asm__ volatile("lv.q C000, 0(%0)\n vf2iz.q C020, C000, 0\n sv.q C020, 0(%1)" : : "r"(halves), "r"(ints) : "memory");
    near("vf2iz.q x", (float)ints[0], 1.0f, 0);
    near("vf2iz.q w", (float)ints[3], -2.0f, 0);
    __asm__ volatile("lv.q C000, 0(%0)\n vf2in.q C020, C000, 0\n sv.q C020, 0(%1)" : : "r"(halves), "r"(ints) : "memory");
    near("vf2in.q x (to even)", (float)ints[0], 2.0f, 0);
    near("vf2in.q z (to even)", (float)ints[2], 2.0f, 0);
    __asm__ volatile("lv.q C000, 0(%0)\n vf2iz.q C020, C000, 16\n vi2f.q C030, C020, 16\n sv.q C030, 0(%1)"
                     : : "r"(halves), "r"(out) : "memory");
    near("vf2iz/vi2f scale 16 round trip", out[0], 1.5f, 1e-4f);

    // Matrices: vmidt, vmmul, vtfm4, vmscl.
    __asm__ volatile("vmidt.q M100\n sv.q C100, 0(%0)\n sv.q C130, 16(%0)" : : "r"(out) : "memory");
    near("vmidt diagonal", out[0], 1.0f, 0);
    near("vmidt off-diagonal", out[4], 0.0f, 0);
    __asm__ volatile("lv.q C000, 0(%0)\n lv.q C010, 16(%0)\n lv.q C020, 32(%0)\n lv.q C030, 48(%0)\n"
                     "lv.q C100, 0(%1)\n vtfm4.q C200, M000, C100\n sv.q C200, 0(%2)"
                     : : "r"(mat), "r"(in_a), "r"(out) : "memory");
    // vtfm dots each column of the matrix register with the vector (the SDK's gumFastInverse relies on this:
    // it transforms the translation by M200 to get R^T * t).
    near("vtfm4 x", out[0], 1.0f * 1, 1e-4f);
    near("vtfm4 y", out[1], 2.0f * 2, 1e-4f);
    near("vtfm4 z", out[2], 3.0f * 3, 1e-4f);
    near("vtfm4 w", out[3], 10.0f * 1 + 20.0f * 2 + 30.0f * 3 + 1.0f * 4, 1e-4f);
    __asm__ volatile("vmmul.q M100, M000, M000\n sv.q C130, 0(%0)" : : "r"(out) : "memory");
    // Column 3 of (M*M) with M as loaded: translation (10,20,30) transformed by M plus its own translation.
    near("vmmul translation x", out[0], 10.0f * 1 + 10.0f, 1e-3f);
    near("vmmul translation y", out[1], 20.0f * 2 + 20.0f, 1e-3f);
    near("vmmul translation z", out[2], 30.0f * 3 + 30.0f, 1e-3f);

    // Cross product and compare/move.
    static float ex[4] __attribute__((aligned(16))) = {1, 0, 0, 0};
    static float ey[4] __attribute__((aligned(16))) = {0, 1, 0, 0};
    __asm__ volatile("lv.q C000, 0(%0)\n lv.q C010, 0(%1)\n vcrsp.t C020, C000, C010\n sv.q C020, 0(%2)"
                     : : "r"(ex), "r"(ey), "r"(out) : "memory");
    near("vcrsp z", out[2], 1.0f, 0);
    near("vcrsp x", out[0], 0.0f, 0);
    __asm__ volatile(LOAD_AB "vcmp.q GT, C000, C010\n vzero.q C020\n vcmovt.q C020, C000, 4\n sv.q C020, 0(%2)"
                     : : "r"(in_a), "r"(in_b), "r"(out) : "memory");
    // vcmovt with cc 4 ("any"): every lane where... here: all lanes of a > b are true -> copies all.
    near("vcmp+vcmovt", out[0], 1.0f, 0);

    // Scalar moves between GPRs and the VFPU.
    unsigned bits = 0x40490FDBu, back = 0;
    __asm__ volatile("mtv %1, S000\n mfv %0, S000" : "=r"(back) : "r"(bits));
    near("mtv/mfv", back == bits ? 1.0f : 0.0f, 1.0f, 0);
    __asm__ volatile("vcst.s S000, VFPU_PI\n sv.s S000, 0(%0)" : : "r"(out) : "memory");
    near("vcst pi", out[0], 3.14159265f, 1e-6f);

    // Unaligned quad load/store (lvl.q/lvr.q, svl.q/svr.q), as the SDK's gum functions use them.
    static float buffer[12] __attribute__((aligned(16))) = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    float *p = &buffer[1]; // 4-byte aligned, not 16
    __asm__ volatile("lvl.q C000, 12(%0)\n lvr.q C000, 0(%0)\n sv.q C000, 0(%1)" : : "r"(p), "r"(out) : "memory");
    near("lvl/lvr x", out[0], 1.0f, 0);
    near("lvl/lvr w", out[3], 4.0f, 0);
    __asm__ volatile("vone.q C000\n svl.q C000, 28(%0)\n svr.q C000, 16(%0)" : : "r"(p) : "memory");
    near("svl/svr inside", buffer[5], 1.0f, 0);
    near("svl/svr end", buffer[8], 1.0f, 0);
    near("svl/svr untouched", buffer[9], 9.0f, 0);

    printf("vfputest: %d passed, %d failed\n", passed, failed);
    sceKernelExitGame();
    return 0;
}
