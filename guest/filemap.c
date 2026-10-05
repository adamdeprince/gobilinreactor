#include "test_syscall.h"
__attribute__((noreturn, used)) static void run(const ulong* stack) {
    ulong ps = auxv(stack, 6);
    long fd = sys6(56, -100, (long)"/tmp/mapping", 0102 | 01000, 0600, 0, 0);
    CHECK(fd >= 0, 1);
    CHECK(sys6(46, fd, ps * 2 + 17, 0, 0, 0, 0) == 0, 2);
    volatile char* shared = (void*)sys6(222, 0, ps * 4, 3, 1, fd, 0);
    volatile char* second = (void*)sys6(222, 0, ps * 3, 3, 1, fd, 0);
    volatile char* private = (void*)sys6(222, 0, ps * 4, 3, 2, fd, 0);
    CHECK((long)shared > 0 && (long)second > 0 && (long)private > 0, 3);
    shared[ps] = 42;
    CHECK(second[ps] == 42 && private[ps] == 42, 4);
    char byte = 0;
    CHECK(sys6(67, fd, (long)&byte, 1, ps, 0, 0) == 1 && byte == 42, 5);
    private[ps] = 99;
    CHECK(shared[ps] == 42, 6);
    CHECK(sys6(233, (long)private + ps, ps, 4, 0, 0, 0) == 0 && private[ps] == 42, 7);
    CHECK(sys6(227, (long)shared, ps * 3, 4, 0, 0, 0) == 0, 8);
    long child = sys6(220, 17, 0, 0, 0, 0, 0);
    CHECK(child >= 0, 9);
    if (!child) {
        long pause[] = {0, 20000000}; sys6(101, (long)pause, 0, 0, 0, 0, 0);
        shared[0] = 11; private[ps] = 88;
        __atomic_store_n((volatile int*)(shared + 8), 1, __ATOMIC_RELEASE);
        sys6(98, (long)shared + 8, 1, 1, 0, 0, 0);
        sys6(93, 0, 0, 0, 0, 0, 0);
    }
    long timeout[] = {1, 0};
    long waited = sys6(98, (long)shared + 8, 0, 0, (long)timeout, 0, 0);
    CHECK(waited == 0 || waited == -11, 24);
    int status;
    CHECK(sys6(260, child, (long)&status, 0, 0, 0, 0) == child && status == 0, 10);
    CHECK(shared[0] == 11 && private[ps] == 42, 11);
    CHECK(shared[ps * 2 + 100] == 0, 12);
    child = sys6(220, 17, 0, 0, 0, 0, 0);
    CHECK(child >= 0, 13);
    if (!child) { byte = shared[ps * 3]; sys6(93, 99, 0, 0, 0, 0, 0); }
    CHECK(sys6(260, child, (long)&status, 0, 0, 0, 0) == child && status == 7, 14);
    CHECK(sys6(46, fd, ps, 0, 0, 0, 0) == 0, 15);
    child = sys6(220, 17, 0, 0, 0, 0, 0);
    CHECK(child >= 0, 16);
    if (!child) { byte = private[ps]; sys6(93, 99, 0, 0, 0, 0, 0); }
    CHECK(sys6(260, child, (long)&status, 0, 0, 0, 0) == child && status == 7, 17);
    CHECK(sys6(46, fd, ps * 4, 0, 0, 0, 0) == 0, 18);
    CHECK(shared[ps * 3] == 0, 19);
    CHECK(sys6(57, fd, 0, 0, 0, 0, 0) == 0, 20);
    shared[ps * 3] = 71;
    CHECK(sys6(227, (long)shared, ps * 4, 4, 0, 0, 0) == 0, 21);
    fd = sys6(56, -100, (long)"/tmp/mapping", 0, 0, 0, 0);
    CHECK(sys6(67, fd, (long)&byte, 1, ps * 3, 0, 0) == 1 && byte == 71, 22);
    CHECK(sys6(222, 0, ps, 3, 1, fd, 0) == -13, 23);
    DONE(0, "file mapping coherence and faults ok\n");
}
__attribute__((naked, noreturn)) void _start(void) {
    __asm__ volatile("mov x0, sp\nb run");
}
