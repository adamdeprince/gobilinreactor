#include "test_syscall.h"
static volatile int image_data = 7;
static void unwanted(int sig) { (void)sig; }
__attribute__((noreturn, used)) void test_main(const ulong* stack) {
    if (stack[0] == 2) {
        CHECK(image_data == 7, 1);
        CHECK(sys6(25, 19, 1, 0, 0, 0, 0) == -9, 2);
        CHECK(sys6(25, 20, 1, 0, 0, 0, 0) == 0, 3);
        CHECK(sys6(172, 0, 0, 0, 0, 0, 0) == 2, 4);
        struct { ulong handler, flags, restorer, mask; } action;
        CHECK(sys6(134, 10, 0, (long)&action, 8, 0, 0) == 0 && action.handler == 0, 5);
        CHECK(sys6(134, 12, 0, (long)&action, 8, 0, 0) == 0 && action.handler == 1, 6);
        const char message[] = "exec descriptors and signals ok\n";
        CHECK(sys6(64, 20, (long)message, sizeof(message) - 1, 0, 0, 0) == sizeof(message) - 1, 7);
        sys6(94, 0, 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }
    struct { ulong handler, flags, restorer, mask; } action;
    action.handler = (ulong)&unwanted; action.flags = 0; action.restorer = 0; action.mask = 0;
    CHECK(sys6(134, 10, (long)&action, 0, 8, 0, 0) == 0, 8);
    action.handler = 1;
    CHECK(sys6(134, 12, (long)&action, 0, 8, 0, 0) == 0, 9);
    int pipefd[2];
    CHECK(sys6(59, (long)pipefd, 0, 0, 0, 0, 0) == 0, 10);
    CHECK(sys6(24, pipefd[1], 19, 0x80000, 0, 0, 0) == 19, 11);
    CHECK(sys6(24, pipefd[1], 20, 0, 0, 0, 0) == 20, 12);
    CHECK(sys6(57, pipefd[1], 0, 0, 0, 0, 0) == 0, 13);
    image_data = 99;
    long child = sys6(220, 17, 0, 0, 0, 0, 0);
    CHECK(child >= 0, 14);
    if (!child) {
        const char path[] = "/tmp/fdexec";
        const char argument[] = "child";
        const char* arguments[] = {path, argument, 0};
        const char* environment[] = {0};
        sys6(221, (long)path, (long)arguments, (long)environment, 0, 0, 0);
        DONE(15, "exec failed\n");
    }
    CHECK(sys6(57, 19, 0, 0, 0, 0, 0) == 0 && sys6(57, 20, 0, 0, 0, 0, 0) == 0, 16);
    char buffer[64];
    long n = sys6(63, pipefd[0], (long)buffer, sizeof(buffer), 0, 0, 0);
    CHECK(n > 0, 17);
    int status;
    CHECK(sys6(260, child, (long)&status, 0, 0, 0, 0) == child && status == 0, 18);
    CHECK(image_data == 99, 19);
    CHECK(sys6(64, 1, (long)buffer, n, 0, 0, 0) == n, 20);
    sys6(94, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
__asm__(".global _start\n_start:\n mov x0, sp\n b test_main\n");
