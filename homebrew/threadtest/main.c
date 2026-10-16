// threadtest: checks kernel scheduling and synchronization semantics in the runtime (docs/05-runtime-hle.md §2).
// Our own code; GPL-2.0-only like the rest of the repository.
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("threadtest", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_MAIN_THREAD_PRIORITY(0x30);

static int passed, failed;
static char order[64];
static int order_len;

static void mark(char c) { if (order_len < 63) order[order_len++] = c; order[order_len] = 0; }
static void reset_order(void) { order_len = 0; order[0] = 0; }

static void check(const char *name, int got, int expected) {
    if (got == expected) { ++passed; } else { ++failed; printf("FAIL %s: got %d (0x%08x) expected %d\n", name, got, got, expected); }
}
static void check_order(const char *name, const char *expected) {
    if (strcmp(order, expected) == 0) { ++passed; } else { ++failed; printf("FAIL %s: got \"%s\" expected \"%s\"\n", name, order, expected); }
}

static SceUID sema, flag;

static int sema_waiter(SceSize args, void *argp) {
    mark('a');
    sceKernelWaitSema(sema, 1, NULL);
    mark('b');
    return 7;
}

static int flag_waiter(SceSize args, void *argp) {
    u32 bits = 0;
    int r = sceKernelWaitEventFlag(flag, 0x3, PSP_EVENT_WAITAND | PSP_EVENT_WAITCLEAR, &bits, NULL);
    mark(r == 0 && bits == 0x7 ? 'F' : 'x');
    return 0;
}

static int delayer(SceSize args, void *argp) {
    int usec = *(int *)argp;
    sceKernelDelayThread(usec);
    mark(usec < 20000 ? '1' : '2');
    return usec;
}

static int sleeper(SceSize args, void *argp) {
    sceKernelSleepThread(); // consumes the pending wakeup immediately
    mark('s');
    sceKernelSleepThread(); // really sleeps
    mark('w');
    return 0;
}

static SceLwMutexWorkarea mutex;
static int shared_counter;

static int lw_worker(SceSize args, void *argp) {
    for (int i = 0; i < 100; ++i) {
        sceKernelLockLwMutex(&mutex, 1, NULL);
        int v = shared_counter;
        if ((i & 7) == 0) sceKernelDelayThread(10); // yield while holding the lock
        shared_counter = v + 1;
        sceKernelUnlockLwMutex(&mutex, 1);
    }
    return 0;
}

static int callback_count, callback_arg;
static SceUID main_thread, callback_id;

static int on_callback(int count, int argument, void *common) {
    callback_count += count;
    callback_arg = argument;
    if (argument == 2) sceKernelWakeupThread(main_thread);
    return 0;
}

static int notifier(SceSize args, void *argp) {
    sceKernelDelayThread(5000);
    sceKernelNotifyCallback(callback_id, 2);
    return 0;
}

int main(void) {
    // Preemption: a higher-priority thread waiting on a semaphore runs as soon as it is signaled.
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    SceUID t = sceKernelCreateThread("high", sema_waiter, 0x20, 0x1000, 0, NULL);
    reset_order();
    sceKernelStartThread(t, 0, NULL); // runs immediately until it blocks
    mark('m');
    sceKernelSignalSema(sema, 1);     // wakes 'high', which preempts us
    mark('n');
    check_order("preemption", "ambn");
    check("wait thread end status", sceKernelWaitThreadEnd(t, NULL), 7);
    check("delete thread", sceKernelDeleteThread(t), 0);

    // Semaphore errors and polling.
    check("poll empty sema", sceKernelPollSema(sema, 1), (int)0x800201AD);
    check("signal overflow", sceKernelSignalSema(sema, 2), (int)0x800201AE);
    SceUInt timeout = 2000;
    check("wait sema timeout", sceKernelWaitSema(sema, 1, &timeout), (int)0x800201A8);
    check("timeout written back", (int)timeout, 0);

    // Event flags: AND wait with clear.
    flag = sceKernelCreateEventFlag("flag", 0, 0, NULL);
    t = sceKernelCreateThread("flag", flag_waiter, 0x20, 0x1000, 0, NULL);
    reset_order();
    sceKernelStartThread(t, 0, NULL);
    sceKernelSetEventFlag(flag, 0x1);   // not enough
    mark('m');
    sceKernelSetEventFlag(flag, 0x6);   // bits 0x7 now: satisfies 0x3 AND
    check_order("event flag and", "mF");
    u32 bits = 0xFFFF;
    check("flag cleared by waiter", sceKernelPollEventFlag(flag, 0x1, PSP_EVENT_WAITOR, &bits), (int)0x800201AF);
    check("poll reports bits", (int)bits, 0);
    sceKernelWaitThreadEnd(t, NULL);
    sceKernelDeleteThread(t);

    // Delays: the shorter delay finishes first.
    static int long_delay = 60000, short_delay = 10000; // far apart, so host load cannot reorder them
    SceUID d1 = sceKernelCreateThread("d1", delayer, 0x20, 0x1000, 0, NULL);
    SceUID d2 = sceKernelCreateThread("d2", delayer, 0x20, 0x1000, 0, NULL);
    reset_order();
    sceKernelStartThread(d1, sizeof long_delay, &long_delay);
    sceKernelStartThread(d2, sizeof short_delay, &short_delay);
    check("delay exit status", sceKernelWaitThreadEnd(d1, NULL), 60000);
    sceKernelWaitThreadEnd(d2, NULL);
    check_order("delay order", "12");

    // Sleep / wakeup counting.
    SceUID s = sceKernelCreateThread("sleeper", sleeper, 0x40, 0x1000, 0, NULL); // lower priority than main
    reset_order();
    sceKernelStartThread(s, 0, NULL);
    sceKernelWakeupThread(s);       // counted: the first sleep returns at once
    sceKernelDelayThread(1000);     // let it run
    mark('m');
    sceKernelWakeupThread(s);
    sceKernelDelayThread(1000);
    check_order("sleep/wakeup", "smw");

    // Lightweight mutex across two equal-priority threads.
    check("create lwmutex", sceKernelCreateLwMutex(&mutex, "lw", 0, 0, NULL), 0);
    SceUID w1 = sceKernelCreateThread("w1", lw_worker, 0x20, 0x1000, 0, NULL);
    SceUID w2 = sceKernelCreateThread("w2", lw_worker, 0x20, 0x1000, 0, NULL);
    sceKernelStartThread(w1, 0, NULL);
    sceKernelStartThread(w2, 0, NULL);
    sceKernelWaitThreadEnd(w1, NULL);
    sceKernelWaitThreadEnd(w2, NULL);
    check("lwmutex protects counter", shared_counter, 200);
    check("unlock not owner", sceKernelUnlockLwMutex(&mutex, 1), (int)0x800201CC);

    // Time moves forward.
    SceKernelSysClock c1, c2;
    sceKernelGetSystemTime(&c1);
    sceKernelDelayThread(5000);
    sceKernelGetSystemTime(&c2);
    check("system time advances", c2.low - c1.low >= 5000, 1);

    // Callbacks: run in *CB waits, also when notified while the thread is already waiting.
    main_thread = sceKernelGetThreadId();
    callback_id = sceKernelCreateCallback("cb", on_callback, NULL);
    sceKernelNotifyCallback(callback_id, 1);
    check("callback not run outside CB waits", callback_count, 0);
    sceKernelDelayThreadCB(1000);
    check("callback runs in a CB wait", callback_count, 1);
    check("callback argument", callback_arg, 1);
    SceUID n = sceKernelCreateThread("notifier", notifier, 0x30, 0x1000, 0, NULL);
    sceKernelStartThread(n, 0, NULL);
    sceKernelSleepThreadCB(); // woken by the callback, which runs during the sleep
    check("callback interrupts a CB wait", callback_count, 2);
    check("callback argument 2", callback_arg, 2);
    sceKernelNotifyCallback(callback_id, 3);
    check("check callback", sceKernelCheckCallback(), 1);
    check("callback count 3", callback_count, 3);

    printf("threadtest: %d passed, %d failed\n", passed, failed);
    sceKernelExitGame();
    return 0;
}
