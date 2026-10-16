// hello: smallest program for the MH-Forever PSP runtime (phase 1).
// Prints through stdout (sceIoWrite) and exits. Our own code; GPL-2.0-only like the rest of the repository.
#include <pspkernel.h>
#include <stdio.h>

PSP_MODULE_INFO("hello", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

int main(void) {
    printf("hello from the PSP runtime\n");
    for (int i = 1; i <= 3; ++i) {
        printf("count %d\n", i);
    }
    sceKernelExitGame();
    return 0;
}
