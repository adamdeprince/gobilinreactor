// Phase 1 guest program.
//
// Freestanding: no libc, no dynamic linker, no startup files. Every syscall is a
// raw `svc #0`, which is precisely what the sentry has to intercept. Being this
// small makes it unambiguous -- if "hello, world" comes out of the sentry's
// write(2) handler, the whole path worked: ELF loaded into sealed executable
// memory, seccomp trapped the guest, and the syscall was serviced in userspace.

#define SYS_write      64
#define SYS_exit_group 94

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
    static const char msg[] = "hello, world\n";
    sys3(SYS_write, 1, (long)msg, sizeof(msg) - 1);
    sys3(SYS_exit_group, 0, 0, 0);
    __builtin_unreachable();
}
