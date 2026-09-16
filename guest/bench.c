// Phase 1 benchmark guest.
//
// Hammers one trivial syscall so the harness can report what a guest syscall
// actually costs through the whole sentry path: the seccomp trap, signal
// delivery, dispatch, and the return back into guest code. The probe measured
// the bare trap at ~780 ns; the gap between the two numbers is the sentry's own
// overhead, which is the part we control.

#define SYS_getpid     172
#define SYS_write       64
#define SYS_exit_group  94

#define ITERATIONS 200000

static long sys3(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2)
                     : "memory", "cc");
    return x0;
}

__attribute__((noreturn)) void _start(void) {
    long acc = 0;
    for (int i = 0; i < ITERATIONS; ++i) acc += sys3(SYS_getpid, 0, 0, 0);

    static const char msg[] = "bench done\n";
    sys3(SYS_write, 1, (long)msg, sizeof(msg) - 1);
    // Exit non-zero if the guest ever saw a pid it should not have, so a broken
    // return path cannot pass silently.
    sys3(SYS_exit_group, acc == (long)ITERATIONS ? 0 : 1, 0, 0);
    __builtin_unreachable();
}
