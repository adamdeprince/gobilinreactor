#include "test_syscall.h"
__attribute__((noreturn, used)) void test_main(const ulong* stack) {
    const ulong gate = auxv(stack, 0x6000);
    const ulong state = auxv(stack, 0x6002);
    CHECK(gate && state, 1);
    // No valid bionic TLS remains. Exercise both trapped syscalls and raw
    // helper failures after explicitly changing the thread pointer.
    __asm__ volatile("msr tpidr_el0, %0" :: "r"((ulong)1) : "memory");
    CHECK(sys6(172, 0, 0, 0, 0, 0, 0) == 1, 2);
    CHECK(gate6(gate, 172, 0, 0, 0, 0, 0, 0) == 1, 3);
    CHECK(gate6(gate, 98, state, 0, 42, 0, 0, 0) == -11, 4);
    const long base = sys6(214, 0, 0, 0, 0, 0, 0);
    CHECK(gate6(gate, 222, base, 0, 3, 0x32, -1, 0) == -22, 5);
    // An attacker may jump to the real svc, but cannot remap the runtime,
    // protect the channel, use arbitrary futex addresses, or open host files.
    const ulong page = auxv(stack, 6);
    const ulong runtime_page = gate & ~(page - 1);
    CHECK(gate6(gate, 215, runtime_page, page, 0, 0, 0, 0) == -1, 6);
    CHECK(gate6(gate, 226, state, page, 7, 0, 0, 0) == -1, 7);
    CHECK(gate6(gate, 98, base, 1, 1, 0, 0, 0) == -1, 8);
    const char path[] = "/proc/self/status";
    CHECK(gate6(gate, 56, -100, (long)path, 0, 0, 0, 0) == -2, 9);
    CHECK(gate6(gate, 220, 0x10000 | 17, 0, 0, 0, 0, 0) == -22, 10);
    // prctl is virtualized for no_new_privs; option zero is invalid.
    CHECK(gate6(gate, 167, 0, 0, 0, 0, 0, 0) == -22, 11);
    CHECK(sys6(172, 0, 0, 0, 0, 0, 0) == 1, 12);
    DONE(0, "isolation and TLS ok\n");
}
__asm__(".global _start\n_start:\n mov x0, sp\n b test_main\n");
