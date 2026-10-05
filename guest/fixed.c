#include "test_syscall.h"
static volatile int value = 17;
__attribute__((noreturn)) void _start(void) {
    CHECK((ulong)&value >= 0x2200000 && (ulong)&value < 0x4000000, 1);
    const long heap = sys6(214, 0, 0, 0, 0, 0, 0);
    CHECK(heap > 0x2200000 && heap < 0x4000000, 2);
    CHECK(sys6(214, heap + 16384, 0, 0, 0, 0, 0) == heap + 16384, 3);
    *(volatile int*)heap = 23;
    CHECK(sys6(214, 0x5000000, 0, 0, 0, 0, 0) == heap + 16384, 4);
    CHECK(sys6(222, 0x4000000, 16384, 3, 0x32, -1, 0) == -12, 5);
    CHECK(sys6(222, 0x100000, 16384, 3, 0x32, -1, 0) == 0x100000, 6);
    *(volatile int*)0x100000 = 42;
    const long moved = sys6(216, 0x100000, 16384, 32768, 1, 0, 0);
    CHECK(moved > 0x100000000L && *(volatile int*)moved == 42, 7);
    long child = sys6(220, 17, 0, 0, 0, 0, 0);
    CHECK(child >= 0, 8);
    if (!child) {
        CHECK(value == 17 && *(volatile int*)heap == 23, 9);
        value = 99; *(volatile int*)heap = 99;
        sys6(94, 0, 0, 0, 0, 0, 0); __builtin_unreachable();
    }
    int status;
    CHECK(sys6(260, child, (long)&status, 0, 0, 0, 0) == child && !status, 10);
    CHECK(value == 17 && *(volatile int*)heap == 23, 11);
    DONE(0, "fixed ELF low memory and fork ok\n");
}
