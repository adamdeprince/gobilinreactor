#include "test_syscall.h"
static volatile unsigned pages[4096];
__attribute__((noreturn)) void _start(void) {
    for (unsigned i=0;i<4096;i++)pages[i]=i;
    for (unsigned round=0;round<32;round++) {
        long child=sys6(220,17,0,0,0,0,0); CHECK(child>=0,1);
        if (!child) { CHECK(pages[round]==round,2);pages[round]=999;sys6(94,0,0,0,0,0,0);__builtin_unreachable(); }
        int status=-1;CHECK(sys6(260,child,(long)&status,0,0,0,0)==child&&!status,3);
        CHECK(pages[round]==round,4);
    }
    DONE(0,"32 fork/wait and COW checks passed\n");
}
