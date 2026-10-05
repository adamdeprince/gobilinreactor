#include "test_syscall.h"
static volatile int private_value = 7;
__attribute__((noreturn, used)) void test_main(const ulong* stack) {
    const ulong page = auxv(stack, 6);
    volatile int* shared = (volatile int*)sys6(222, 0, page, 3, 0x21, -1, 0);
    CHECK((long)shared > 0, 1);
    int* guard = (int*)sys6(222, 0, page, 3, 0x22, -1, 0);
    CHECK((long)guard > 0, 2); *guard = 99;
    CHECK(sys6(226, (long)guard, page, 0, 0, 0, 0) == 0, 3);
    int pipefd[2];
    CHECK(sys6(59, (long)pipefd, 0, 0, 0, 0, 0) == 0, 4);
    const ulong tls = 0x12345;
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(tls) : "memory");
    int parent_tid = 0;
    long child = sys6(220, 17 | 0x1000000 | 0x200000 | 0x100000,
                      0, (long)&parent_tid, 0, (long)&shared[2], 0);
    CHECK(child >= 0, 5);
    ulong current_tls;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(current_tls));
    CHECK(current_tls == tls, 6);
    if (!child) {
        CHECK(shared[2] == sys6(178, 0, 0, 0, 0, 0, 0), 7);
        CHECK(sys6(226, (long)guard, page, 1, 0, 0, 0) == 0 && *guard == 99, 8);
        private_value = 42;
        CHECK(sys6(57, pipefd[0], 0, 0, 0, 0, 0) == 0, 9);
        CHECK(sys6(24, pipefd[1], 19, 0x80000, 0, 0, 0) == 19, 10);
        CHECK(sys6(25, 19, 1, 0, 0, 0, 0) == 1, 11);
        CHECK(sys6(57, pipefd[1], 0, 0, 0, 0, 0) == 0, 12);
        long delay[] = {0, 20000000};
        CHECK(sys6(101, (long)delay, 0, 0, 0, 0, 0) == 0, 13);
        __atomic_store_n(&shared[0], 42, __ATOMIC_RELEASE);
        CHECK(sys6(98, (long)shared, 1, 1, 0, 0, 0) >= 0, 14);
        const char text[] = "child";
        CHECK(sys6(64, 19, (long)text, 5, 0, 0, 0) == 5, 15);
        sys6(94, 23, 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }
    CHECK(parent_tid == child, 16);
    CHECK(sys6(57, pipefd[1], 0, 0, 0, 0, 0) == 0, 17);
    long timeout[] = {1, 0};
    long waited = sys6(98, (long)shared, 0, 0, (long)timeout, 0, 0);
    CHECK(waited == 0 || waited == -11, 18);
    CHECK(__atomic_load_n(&shared[0], __ATOMIC_ACQUIRE) == 42, 19);
    char text[8] = {};
    CHECK(sys6(63, pipefd[0], (long)text, sizeof(text), 0, 0, 0) == 5, 20);
    CHECK(text[0] == 'c' && text[4] == 'd', 21);
    CHECK(sys6(63, pipefd[0], (long)text, sizeof(text), 0, 0, 0) == 0, 22);
    int status = 0;
    CHECK(sys6(260, child, (long)&status, 0, 0, 0, 0) == child && status == (23 << 8), 23);
    CHECK(shared[2] == child && private_value == 7, 24);
    CHECK(sys6(260, child, (long)&status, 1, 0, 0, 0) == -10, 25);
    CHECK(sys6(226, (long)guard, page, 1, 0, 0, 0) == 0 && *guard == 99, 26);
    timeout[0] = 0; timeout[1] = 1000000;
    CHECK(sys6(98, (long)shared, 0, 42, (long)timeout, 0, 0) == -110, 27);
    CHECK(sys6(98, (long)shared, 0, 0, 0, 0, 0) == -11, 28);
    child = sys6(220, 17, 0, 0, 0, 0, 0);
    CHECK(child >= 0, 29);
    if (!child) {
        long delay[] = {10, 0};
        sys6(101, (long)delay, 0, 0, 0, 0, 0);
        sys6(94, 99, 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }
    CHECK(sys6(129, child, 19, 0, 0, 0, 0) == 0, 30);
    CHECK(sys6(260, child, (long)&status, 2, 0, 0, 0) == child && status == ((19 << 8) | 127), 31);
    CHECK(sys6(129, child, 18, 0, 0, 0, 0) == 0, 32);
    CHECK(sys6(260, child, (long)&status, 8, 0, 0, 0) == child && status == 65535, 33);
    CHECK(sys6(129, child, 15, 0, 0, 0, 0) == 0, 34);
    CHECK(sys6(260, child, (long)&status, 0, 0, 0, 0) == child && status == 15, 35);
    child = sys6(220, 17 | 0x100 | 0x4000, 0, 0, 0, 0, 0);
    CHECK(child >= 0, 36);
    if (!child) {
        private_value = 123;
        sys6(94, 0, 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }
    CHECK(private_value == 123, 37);
    CHECK(sys6(260, child, (long)&status, 0, 0, 0, 0) == child && status == 0, 38);
    DONE(0, "fork pipes shared memory and futex ok\n");
}
__asm__(".global _start\n_start:\n mov x0, sp\n b test_main\n");
