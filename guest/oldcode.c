#include "test_syscall.h"
__attribute__((noreturn, used)) void test_main(const ulong* stack) {
    const ulong old = auxv(stack, 0x6001);
    CHECK(old != 0, 1);
    ((void (*)(void))old)();  // must SIGSEGV: inherited bionic was unmapped
    DONE(2, "inherited code still mapped\n");
}
__asm__(".global _start\n_start:\n mov x0, sp\n b test_main\n");
