#include "test_syscall.h"
static volatile int caught;
static volatile ulong handler_stack;
static void handler(int signal, void* info, void* context) {
    (void)info; (void)context;
    ulong sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    handler_stack = sp;
    caught += signal;
}
__attribute__((noreturn, used)) void test_main(const ulong* stack) {
    ulong page = auxv(stack, 6);
    long memory = sys6(222, 0, page * 8, 3, 0x22, -1, 0);
    CHECK(memory > 0, 1);
    struct { ulong pointer; int flags; int pad; ulong size; } alt = {memory, 0, 0, page * 8};
    CHECK(sys6(132, (long)&alt, 0, 0, 0, 0, 0) == 0, 2);
    struct { ulong handler, flags, restorer, mask; } action;
    action.handler = (ulong)&handler; action.flags = 4 | 0x08000000; action.restorer = 0; action.mask = 0;
    CHECK(sys6(134, 10, (long)&action, 0, 8, 0, 0) == 0, 3);
    ulong mask = 1UL << 9;
    CHECK(sys6(135, 0, (long)&mask, 0, 8, 0, 0) == 0, 4);
    CHECK(sys6(129, 1, 10, 0, 0, 0, 0) == 0 && caught == 0, 5);
    ulong pending = 0;
    CHECK(sys6(136, (long)&pending, 8, 0, 0, 0, 0) == 0 && (pending & mask), 6);
    CHECK(sys6(135, 1, (long)&mask, 0, 8, 0, 0) == 0 && caught == 10, 7);
    CHECK(handler_stack >= (ulong)memory && handler_stack < memory + page * 8, 8);
    long child = sys6(220, 17, 0, 0, 0, 0, 0);
    CHECK(child >= 0, 9);
    if (!child) {
        long delay[] = {0, 20000000};
        sys6(101, (long)delay, 0, 0, 0, 0, 0);
        CHECK(sys6(129, 1, 10, 0, 0, 0, 0) == 0, 10);
        sys6(94, 0, 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }
    long delay[] = {1, 0}, remaining[] = {0, 0};
    CHECK(sys6(101, (long)delay, (long)remaining, 0, 0, 0, 0) == -4, 11);
    CHECK(caught == 20 && (remaining[0] || remaining[1]), 12);
    int status;
    CHECK(sys6(260, child, (long)&status, 0, 0, 0, 0) == child && status == 0, 13);
    // All virtual pid operations stay in the process table.
    CHECK(sys6(129, 999999, 9, 0, 0, 0, 0) == -3, 14);
    DONE(0, "signals masks altstack and EINTR ok\n");
}
__asm__(".global _start\n_start:\n mov x0, sp\n b test_main\n");
