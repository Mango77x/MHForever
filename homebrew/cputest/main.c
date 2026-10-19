// cputest: checks Allegrex instruction semantics in the recompiled runtime (docs/04-recompiler.md §8).
// Every expected value is written by hand from the architecture's definition, so the test does not depend on the
// code under test. Our own code; GPL-3.0-or-later like the rest of the repository.
#include <pspkernel.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("cputest", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

static int passed, failed;

static void check(const char *name, unsigned got, unsigned expected) {
    if (got == expected) {
        ++passed;
    } else {
        ++failed;
        printf("FAIL %s: got %08x expected %08x\n", name, got, expected);
    }
}

static void checkf(const char *name, float got, float expected) {
    union { float f; unsigned u; } a = {got}, b = {expected};
    check(name, a.u, b.u);
}

// Values hidden from the optimizer.
static volatile unsigned v_a = 0x80000001u, v_b = 0x7FFFFFFFu, v_c = 3, v_d = 0xFFFFFFF9u /* -7 */, v_zero = 0;

#define RRR(insn, a, b) ({ unsigned r_; __asm__ volatile(insn " %0, %1, %2" : "=r"(r_) : "r"(a), "r"(b)); r_; })
#define RRI(insn, a, imm) ({ unsigned r_; __asm__ volatile(insn " %0, %1, " #imm : "=r"(r_) : "r"(a)); r_; })
#define RR(insn, a) ({ unsigned r_; __asm__ volatile(insn " %0, %1" : "=r"(r_) : "r"(a)); r_; })

static void test_alu(void) {
    check("addu wrap", RRR("addu", v_a, v_b), 0x00000000u);
    check("subu", RRR("subu", v_c, v_a), 0x80000002u);
    check("and", RRR("and", v_a, v_b), 0x00000001u);
    check("or", RRR("or", v_a, v_c), 0x80000003u);
    check("xor", RRR("xor", v_a, v_b), 0xFFFFFFFEu);
    check("nor", RRR("nor", v_a, v_c), 0x7FFFFFFCu);
    check("slt signed", RRR("slt", v_a, v_c), 1u);
    check("sltu unsigned", RRR("sltu", v_a, v_c), 0u);
    check("slti", RRI("slti", v_d, -6), 1u);
    check("sltiu sign-extended imm", RRI("sltiu", v_c, -1), 1u);
    check("addiu", RRI("addiu", v_c, -4), 0xFFFFFFFFu);
    check("andi zero-extended", RRI("andi", v_d, 0xFFFF), 0x0000FFF9u);
    check("ori", RRI("ori", v_zero, 0x8000), 0x00008000u);
    check("xori", RRI("xori", v_d, 0xFFFF), 0xFFFF0006u);
    check("sll", RRI("sll", v_a, 4), 0x00000010u);
    check("srl", RRI("srl", v_a, 4), 0x08000000u);
    check("sra", RRI("sra", v_a, 4), 0xF8000000u);
    check("rotr", RRI("rotr", v_a, 4), 0x18000000u);
    check("sllv masks shift", RRR("sllv", v_c, v_c) , 0x00000018u);
    check("srav", RRR("srav", v_d, v_c), 0xFFFFFFFFu);
    check("srlv uses low 5 bits", RRR("srlv", v_a, 33u), 0x40000000u);
    check("rotrv", RRR("rotrv", v_a, v_c), 0x30000000u);
    check("max", RRR("max", v_d, v_c), 3u);
    check("min", RRR("min", v_d, v_c), 0xFFFFFFF9u);
    check("clz", RR("clz", v_c), 30u);
    check("clz zero", RR("clz", v_zero), 32u);
    check("clo", RR("clo", v_d), 29u);
    check("seb", RR("seb", 0x000000F0u + v_zero), 0xFFFFFFF0u);
    check("seh", RR("seh", 0x00018000u + v_zero), 0xFFFF8000u);
    check("wsbh", RR("wsbh", 0x11223344u + v_zero), 0x22114433u);
    check("wsbw", RR("wsbw", 0x11223344u + v_zero), 0x44332211u);
    check("bitrev", RR("bitrev", v_a), 0x80000001u);
    check("bitrev 2", RR("bitrev", 0x00000006u + v_zero), 0x60000000u);
    {
        unsigned r;
        __asm__ volatile("ext %0, %1, 4, 8" : "=r"(r) : "r"(0x12345678u + v_zero));
        check("ext", r, 0x67u);
        r = 0xFFFFFFFFu + v_zero;
        __asm__ volatile("ins %0, %1, 8, 4" : "+r"(r) : "r"(0x5u + v_zero));
        check("ins", r, 0xFFFFF5FFu);
    }
    {
        unsigned r = 7;
        __asm__ volatile("movz %0, %1, %2" : "+r"(r) : "r"(v_c), "r"(v_zero));
        check("movz taken", r, 3u);
        r = 7;
        __asm__ volatile("movn %0, %1, %2" : "+r"(r) : "r"(v_c), "r"(v_zero));
        check("movn not taken", r, 7u);
    }
}

static void test_muldiv(void) {
    unsigned hi, lo;
    __asm__ volatile("mult %2, %3\n mfhi %0\n mflo %1" : "=r"(hi), "=r"(lo) : "r"(v_d), "r"(v_b));
    check("mult hi", hi, 0xFFFFFFFCu);
    check("mult lo", lo, 0x80000007u);
    __asm__ volatile("multu %2, %3\n mfhi %0\n mflo %1" : "=r"(hi), "=r"(lo) : "r"(v_d), "r"(v_b));
    check("multu hi", hi, 0x7FFFFFFBu);
    check("multu lo", lo, 0x80000007u);
    __asm__ volatile("div $zero, %2, %3\n mfhi %0\n mflo %1" : "=r"(hi), "=r"(lo) : "r"(v_d), "r"(v_c));
    check("div quotient", lo, 0xFFFFFFFEu);
    check("div remainder", hi, 0xFFFFFFFFu);
    __asm__ volatile("divu $zero, %2, %3\n mfhi %0\n mflo %1" : "=r"(hi), "=r"(lo) : "r"(v_d), "r"(v_c));
    check("divu quotient", lo, 0x55555553u);
    check("divu remainder", hi, 0x00000000u);
    __asm__ volatile("div $zero, %2, %3\n mfhi %0\n mflo %1" : "=r"(hi), "=r"(lo) : "r"(v_c), "r"(v_zero));
    check("div by zero lo", lo, 0xFFFFFFFFu);
    check("div by zero hi", hi, 3u);
    __asm__ volatile("mthi %2\n mtlo %3\n madd %4, %5\n mfhi %0\n mflo %1"
                     : "=r"(hi), "=r"(lo) : "r"(0u + v_zero), "r"(10u + v_zero), "r"(v_d), "r"(v_c));
    check("madd hi", hi, 0xFFFFFFFFu);
    check("madd lo", lo, 0xFFFFFFF5u);
    __asm__ volatile("mthi %2\n mtlo %3\n maddu %4, %5\n mfhi %0\n mflo %1"
                     : "=r"(hi), "=r"(lo) : "r"(0u + v_zero), "r"(10u + v_zero), "r"(v_b), "r"(v_c));
    check("maddu hi", hi, 1u);
    check("maddu lo", lo, 0x80000007u);
    __asm__ volatile("mthi %2\n mtlo %3\n msub %4, %5\n mfhi %0\n mflo %1"
                     : "=r"(hi), "=r"(lo) : "r"(0u + v_zero), "r"(10u + v_zero), "r"(v_d), "r"(v_c));
    check("msub hi", hi, 0u);
    check("msub lo", lo, 31u);
    __asm__ volatile("mthi %2\n mtlo %3\n msubu %4, %5\n mfhi %0\n mflo %1"
                     : "=r"(hi), "=r"(lo) : "r"(0u + v_zero), "r"(10u + v_zero), "r"(v_c), "r"(v_c));
    check("msubu", lo, 1u);
}

static unsigned char buffer[16] __attribute__((aligned(16)));

static void test_memory(void) {
    for (int i = 0; i < 16; ++i) buffer[i] = (unsigned char)(0x10 + i);
    volatile unsigned char *p = buffer;
    unsigned r;
    __asm__ volatile("lb %0, 0(%1)" : "=r"(r) : "r"(p + 15)); check("lb", r, 0x1Fu);
    p[3] = 0x80;
    __asm__ volatile("lb %0, 0(%1)" : "=r"(r) : "r"(p + 3)); check("lb sign", r, 0xFFFFFF80u);
    __asm__ volatile("lbu %0, 0(%1)" : "=r"(r) : "r"(p + 3)); check("lbu", r, 0x80u);
    __asm__ volatile("lh %0, 2(%1)" : "=r"(r) : "r"(p)); check("lh sign", r, 0xFFFF8012u);
    __asm__ volatile("lhu %0, 2(%1)" : "=r"(r) : "r"(p)); check("lhu", r, 0x8012u);
    __asm__ volatile("lw %0, 4(%1)" : "=r"(r) : "r"(p)); check("lw", r, 0x17161514u);
    r = 0xAABBCCDDu + v_zero;
    __asm__ volatile("lwl %0, 4(%1)" : "+r"(r) : "r"(p)); check("lwl", r, 0x14BBCCDDu);
    r = 0xAABBCCDDu + v_zero;
    __asm__ volatile("lwr %0, 5(%1)" : "+r"(r) : "r"(p)); check("lwr", r, 0xAA171615u);
    __asm__ volatile("lwl %0, 8(%1)\n lwr %0, 5(%1)" : "=&r"(r) : "r"(p)); check("lwl+lwr unaligned", r, 0x18171615u);
    __asm__ volatile("swl %0, 9(%1)" : : "r"(0xA1B2C3D4u + v_zero), "r"(p) : "memory");
    check("swl", *(volatile unsigned *)(p + 8), 0x1B1AA1B2u);
    __asm__ volatile("swr %0, 13(%1)" : : "r"(0xA1B2C3D4u + v_zero), "r"(p) : "memory");
    check("swr", *(volatile unsigned *)(p + 12), 0xB2C3D41Cu);
    __asm__ volatile("sh %0, 0(%1)" : : "r"(0x12345678u + v_zero), "r"(p) : "memory");
    check("sh", *(volatile unsigned *)p, 0x80125678u);
    __asm__ volatile("sb %0, 1(%1)" : : "r"(0xFFFFFF99u + v_zero), "r"(p) : "memory");
    check("sb", *(volatile unsigned *)p, 0x80129978u);
}

static void test_branches(void) {
    unsigned r;
    // The delay slot executes whether or not the branch is taken.
    __asm__ volatile(".set noreorder\n"
                     " li %0, 1\n beq $zero, $zero, 1f\n addiu %0, %0, 10\n addiu %0, %0, 100\n"
                     "1:\n .set reorder" : "=&r"(r));
    check("delay slot on taken branch", r, 11u);
    __asm__ volatile(".set noreorder\n"
                     " li %0, 1\n bne $zero, $zero, 1f\n addiu %0, %0, 10\n addiu %0, %0, 100\n"
                     "1:\n .set reorder" : "=&r"(r));
    check("delay slot on fall-through", r, 111u);
    // Likely branches nullify the delay slot when not taken.
    __asm__ volatile(".set noreorder\n"
                     " li %0, 1\n bnel $zero, $zero, 1f\n addiu %0, %0, 10\n addiu %0, %0, 100\n"
                     "1:\n .set reorder" : "=&r"(r));
    check("likely not taken nullifies", r, 101u);
    __asm__ volatile(".set noreorder\n"
                     " li %0, 1\n beql $zero, $zero, 1f\n addiu %0, %0, 10\n addiu %0, %0, 100\n"
                     "1:\n .set reorder" : "=&r"(r));
    check("likely taken runs slot", r, 11u);
    // Branch reads its operands before the delay slot changes them.
    __asm__ volatile(".set noreorder\n"
                     " li %0, 0\n beq %0, $zero, 1f\n li %0, 5\n li %0, 9\n"
                     "1:\n .set reorder" : "=&r"(r));
    check("branch compares before slot", r, 5u);
    // A loop driven by bgtz with the decrement in the delay slot.
    __asm__ volatile(".set noreorder\n"
                     " li $t0, 4\n li %0, 0\n"
                     "2: addiu %0, %0, 3\n bgtz $t0, 2b\n addiu $t0, $t0, -1\n"
                     " .set reorder" : "=&r"(r) : : "t0");
    check("loop with slot decrement", r, 15u);
}

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static int classify(int x) {
    switch (x) {
    case 0: return 10;
    case 1: return 21;
    case 2: return 32;
    case 3: return 43;
    case 4: return 54;
    case 5: return 65;
    case 6: return 76;
    case 7: return 87;
    default: return -1;
    }
}

static int twice(int x) { return 2 * x; }
static int square(int x) { return x * x; }
static int (*const ops[2])(int) = {twice, square};

static jmp_buf jump;
static void jump_back(int value) { longjmp(jump, value); }

static void test_calls(void) {
    check("recursion", (unsigned)fib(20 + (int)v_zero), 6765u);
    unsigned sum = 0;
    for (int i = -1 + (int)v_zero; i < 9; ++i) sum += (unsigned)classify(i);
    check("jump table", sum, 10u + 21 + 32 + 43 + 54 + 65 + 76 + 87 - 2);
    check("function pointer", (unsigned)ops[v_c & 1](7), 49u);
    check("function pointer 2", (unsigned)ops[v_zero & 1](7), 14u);
    volatile int stage = 0;
    const int r = setjmp(jump);
    if (r == 0) {
        stage = 1;
        jump_back(42);
        stage = 2;
    }
    check("setjmp/longjmp", (unsigned)(r * 10 + stage), 421u);
}

static volatile float f_a = 1.5f, f_b = -2.25f, f_nine = 9.0f;

static void test_fpu(void) {
    checkf("add.s", f_a + f_b, -0.75f);
    checkf("sub.s", f_a - f_b, 3.75f);
    checkf("mul.s", f_a * f_b, -3.375f);
    checkf("div.s", f_b / f_a, -1.5f);
    float r;
    __asm__ volatile("sqrt.s %0, %1" : "=f"(r) : "f"(f_nine)); checkf("sqrt.s", r, 3.0f);
    __asm__ volatile("abs.s %0, %1" : "=f"(r) : "f"(f_b)); checkf("abs.s", r, 2.25f);
    __asm__ volatile("neg.s %0, %1" : "=f"(r) : "f"(f_a)); checkf("neg.s", r, -1.5f);
    int i;
    __asm__ volatile("trunc.w.s %1, %2\n mfc1 %0, %1" : "=r"(i), "=f"(r) : "f"(f_b)); check("trunc.w.s", (unsigned)i, 0xFFFFFFFEu);
    __asm__ volatile("floor.w.s %1, %2\n mfc1 %0, %1" : "=r"(i), "=f"(r) : "f"(f_b)); check("floor.w.s", (unsigned)i, 0xFFFFFFFDu);
    __asm__ volatile("ceil.w.s %1, %2\n mfc1 %0, %1" : "=r"(i), "=f"(r) : "f"(f_a)); check("ceil.w.s", (unsigned)i, 2u);
    __asm__ volatile("round.w.s %1, %2\n mfc1 %0, %1" : "=r"(i), "=f"(r) : "f"(f_a)); check("round.w.s", (unsigned)i, 2u);
    __asm__ volatile("cvt.w.s %1, %2\n mfc1 %0, %1" : "=r"(i), "=f"(r) : "f"(f_a)); check("cvt.w.s nearest even", (unsigned)i, 2u);
    __asm__ volatile("mtc1 %1, %0\n cvt.s.w %0, %0" : "=f"(r) : "r"(v_d)); checkf("cvt.s.w", r, -7.0f);
    unsigned c;
    __asm__ volatile(".set noreorder\n c.lt.s %1, %2\n li %0, 0\n bc1t 1f\n nop\n li %0, 1\n1:\n .set reorder"
                     : "=&r"(c) : "f"(f_b), "f"(f_a));
    check("c.lt.s + bc1t", c, 0u);
    __asm__ volatile(".set noreorder\n c.eq.s %1, %2\n li %0, 0\n bc1f 1f\n nop\n li %0, 1\n1:\n .set reorder"
                     : "=&r"(c) : "f"(f_b), "f"(f_a));
    check("c.eq.s + bc1f", c, 0u);
}

static float vec_a[4] __attribute__((aligned(16))) = {1.0f, 2.0f, 3.0f, 4.0f};
static float vec_b[4] __attribute__((aligned(16))) = {0.5f, -1.0f, 2.0f, 0.25f};
static float vec_out[4] __attribute__((aligned(16)));

static void test_vfpu(void) {
    __asm__ volatile("lv.q C000, 0(%0)\n lv.q C010, 0(%1)\n vadd.q C020, C000, C010\n sv.q C020, 0(%2)"
                     : : "r"(vec_a), "r"(vec_b), "r"(vec_out) : "memory");
    checkf("vadd.q x", vec_out[0], 1.5f);
    checkf("vadd.q w", vec_out[3], 4.25f);
    __asm__ volatile("lv.q C000, 0(%0)\n lv.q C010, 0(%1)\n vmul.q C020, C000, C010\n sv.q C020, 0(%2)"
                     : : "r"(vec_a), "r"(vec_b), "r"(vec_out) : "memory");
    checkf("vmul.q y", vec_out[1], -2.0f);
    __asm__ volatile("lv.q C000, 0(%0)\n lv.q C010, 0(%1)\n vdot.q S020, C000, C010\n sv.s S020, 0(%2)"
                     : : "r"(vec_a), "r"(vec_b), "r"(vec_out) : "memory");
    checkf("vdot.q", vec_out[0], 0.5f - 2.0f + 6.0f + 1.0f);
    __asm__ volatile("lv.q C000, 0(%0)\n vscl.q C020, C000, S010\n sv.q C020, 0(%1)"
                     : : "r"(vec_a), "r"(vec_out) : "memory"); // S010 = vec_b[0] = 0.5 from the previous load
    checkf("vscl.q z", vec_out[2], 1.5f);
    __asm__ volatile("vone.s S000\n vzero.s S001\n sv.s S000, 0(%0)\n sv.s S001, 4(%0)" : : "r"(vec_out) : "memory");
    checkf("vone.s", vec_out[0], 1.0f);
    checkf("vzero.s", vec_out[1], 0.0f);
}

int main(void) {
    test_alu();
    test_muldiv();
    test_memory();
    test_branches();
    test_calls();
    test_fpu();
    test_vfpu();
    printf("cputest: %d passed, %d failed\n", passed, failed);
    sceKernelExitGame();
    return 0;
}
