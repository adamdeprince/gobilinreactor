#include "test_syscall.h"
struct timeval { long sec, usec; };
__attribute__((noreturn)) void _start(void) {
    struct timeval first, next;
    CHECK(sys6(169, (long)&first, 0, 0, 0, 0, 0) == 0, 1);
    CHECK(first.sec > 0 && first.usec >= 0 && first.usec < 1000000, 2);
    CHECK(sys6(169, (long)&next, 0, 0, 0, 0, 0) == 0, 3);
    CHECK(next.sec > first.sec || (next.sec == first.sec && next.usec >= first.usec), 4);
    CHECK(sys6(169, 0, 0, 0, 0, 0, 0) == 0, 5);
    CHECK(sys6(169, 1, 0, 0, 0, 0, 0) == -14, 6);
    int timezone[2] = {123,456};
    CHECK(sys6(169, (long)&next, (long)timezone, 0, 0, 0, 0) == 0, 7);
    CHECK(timezone[0] == 0 && timezone[1] == 0, 8);
    long memory = sys6(222, 0, 16384, 3, 0x22, -1, 0);
    CHECK(memory > 0, 9);
    CHECK(sys6(169, memory + 16384 - 16, 0, 0, 0, 0, 0) == 0, 10);
    CHECK(sys6(226, memory, 16384, 1, 0, 0, 0) == 0, 11);
    CHECK(sys6(169, memory, 0, 0, 0, 0, 0) == -14, 12);
    CHECK(sys6(215, memory, 16384, 0, 0, 0, 0) == 0, 13);
    CHECK(sys6(169, memory, 0, 0, 0, 0, 0) == -14, 14);
    DONE(0, "time query ok\n");
}
