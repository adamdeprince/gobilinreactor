#include "test_syscall.h"
__attribute__((noreturn)) void _start(void) {
    const long base = sys6(214, 0, 0, 0, 0, 0, 0);
    CHECK(base > 0, 1);
    CHECK(sys6(214, base + 1, 0, 0, 0, 0, 0) == base + 1, 2);
    CHECK(sys6(214, base + 2, 0, 0, 0, 0, 0) == base + 2, 3);
    CHECK(sys6(214, base + 1, 0, 0, 0, 0, 0) == base + 1, 4);
    CHECK(sys6(222, 0, -1, 3, 0x22, -1, 0) == -12, 5);
    CHECK(sys6(222, base + 1, 16384, 3, 0x32, -1, 0) == -22, 6);
    CHECK(sys6(222, 0, 16384, 3, 0x20, -1, 0) == -22, 7);
    CHECK(sys6(222, 0, 16384, 0x40000000, 0x22, -1, 0) == -22, 8);
    CHECK(sys6(222, 0, 16384, 3, 0x122, -1, 0) == -22, 9);
    // Linux ignores fd for anonymous mappings.
    const long p = sys6(222, 0, 16384, 3, 0x22, 123, 0);
    CHECK(p > 0, 10);
    CHECK(sys6(222, p, 16384, 3, 0x100022, -1, 0) == -17, 11);
    *(volatile unsigned char*)p = 42;
    CHECK(sys6(233, p, 16384, 4, 0, 0, 0) == 0, 12);
    CHECK(*(volatile unsigned char*)p == 0, 13);
    CHECK(sys6(233, p, 16384, 0x7fffffff, 0, 0, 0) == -22, 14);
    CHECK(sys6(226, p, 16384, 0x40000000, 0, 0, 0) == -22, 15);
    *(volatile unsigned char*)p = 99;
    CHECK(sys6(215, p, 16384, 0, 0, 0, 0) == 0, 16);
    CHECK(sys6(226, p, 16384, 1, 0, 0, 0) == -12, 17);
    CHECK(sys6(233, p, 16384, 4, 0, 0, 0) == -12, 18);
    CHECK(sys6(215, p, 16384, 0, 0, 0, 0) == 0, 19);
    CHECK(sys6(222, p, 16384, 3, 0x100022, -1, 0) == p, 20);
    CHECK(*(volatile unsigned char*)p == 0, 21);
    // MADV_FREE is rejected by the host on a shared mapping. The failure must
    // cross the acknowledgement channel as EINVAL, with the process alive.
    const long shared = sys6(222, 0, 16384, 3, 0x21, -1, 0);
    CHECK(shared > 0, 22);
    *(volatile unsigned char*)shared = 77;
    CHECK(sys6(233, shared, 16384, 8, 0, 0, 0) == -22, 23);
    CHECK(*(volatile unsigned char*)shared == 77, 24);
    const long source = sys6(222, 0, 16384, 3, 0x22, -1, 0);
    CHECK(source > 0, 25);
    *(volatile unsigned char*)source = 43;
    const long moved = sys6(216, source, 16384, 32768, 1, 0, 0);
    CHECK(moved > 0 && moved != source && *(volatile unsigned char*)moved == 43, 26);
    CHECK(*(volatile unsigned char*)(moved + 16384) == 0, 27);
    CHECK(sys6(226, source, 16384, 1, 0, 0, 0) == -12, 28);
    CHECK(sys6(216, moved, 32768, 16384, 0, 0, 0) == moved, 29);
    CHECK(*(volatile unsigned char*)moved == 43 && sys6(226, moved + 16384, 16384, 1, 0, 0, 0) == -12, 30);
    CHECK(sys6(216, moved + 1, 16384, 32768, 1, 0, 0) == -22, 31);
    CHECK(sys6(216, moved, 16384, -1, 1, 0, 0) == -22, 32);
    CHECK(sys6(216, moved, 16384, 32768, 0, 0, 0) == -12, 33);
    CHECK(sys6(216, moved, 16384, 32768, 3, source, 0) == -95, 34);
    DONE(0, "memory errors ok\n");
}
