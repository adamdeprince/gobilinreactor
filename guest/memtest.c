// Phase 2 guest: does the guest have a real address space?
//
// Every step here is a syscall the sentry has to decide on and the stub has to
// carry out in its own address space. If the split between "the sentry chooses"
// and "the stub maps" were wrong anywhere, this faults instead of printing.
//
// Exits with a distinct code per step so a failure says which one.

#define SYS_write       64
#define SYS_exit_group  94
#define SYS_brk        214
#define SYS_munmap     215
#define SYS_mmap       222
#define SYS_mprotect   226

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

typedef unsigned long ulong;

static long sys6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory", "cc");
    return x0;
}

#define SYS0(nr)                   sys6((nr), 0, 0, 0, 0, 0, 0)
#define SYS1(nr, a)                sys6((nr), (long)(a), 0, 0, 0, 0, 0)
#define SYS2(nr, a, b)             sys6((nr), (long)(a), (long)(b), 0, 0, 0, 0)
#define SYS3(nr, a, b, c)          sys6((nr), (long)(a), (long)(b), (long)(c), 0, 0, 0)

static int failed(long v) { return (ulong)v >= (ulong)-4095L; }

__attribute__((noreturn)) static void finish(int code, const char* msg, long len) {
    sys6(SYS_write, 1, (long)msg, len, 0, 0, 0);
    sys6(SYS_exit_group, code, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

#define FAIL(code, text) finish((code), (text), sizeof(text) - 1)

__attribute__((noreturn)) void _start(void) {
    const ulong kHeapGrowth = 0x10000;  // 64 KiB
    const ulong kMapLen = 0x4000;       // 16 KiB

    // --- the heap moves ---
    const long start_brk = SYS1(SYS_brk, 0);
    if (failed(start_brk)) FAIL(10, "brk(0) failed\n");

    const long new_brk = SYS1(SYS_brk, (ulong)start_brk + kHeapGrowth);
    if (failed(new_brk) || (ulong)new_brk < (ulong)start_brk + kHeapGrowth) {
        FAIL(11, "brk could not grow the heap\n");
    }

    volatile unsigned char* heap = (volatile unsigned char*)(ulong)start_brk;
    for (ulong i = 0; i < kHeapGrowth; i += 512) heap[i] = (unsigned char)(i / 512);
    for (ulong i = 0; i < kHeapGrowth; i += 512) {
        if (heap[i] != (unsigned char)(i / 512)) FAIL(12, "heap did not hold its contents\n");
    }

    // --- an anonymous mapping appears where the sentry put it ---
    const long raw = sys6(SYS_mmap, 0, kMapLen, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (failed(raw)) FAIL(20, "mmap failed\n");

    volatile unsigned char* p = (volatile unsigned char*)(ulong)raw;
    for (ulong i = 0; i < kMapLen; i += 256) p[i] = (unsigned char)(i / 256 + 7);
    for (ulong i = 0; i < kMapLen; i += 256) {
        if (p[i] != (unsigned char)(i / 256 + 7)) FAIL(21, "mapping did not hold its contents\n");
    }

    // --- dropping write permission keeps the data readable ---
    if (failed(SYS3(SYS_mprotect, raw, kMapLen, PROT_READ))) {
        FAIL(30, "mprotect failed\n");
    }
    for (ulong i = 0; i < kMapLen; i += 256) {
        if (p[i] != (unsigned char)(i / 256 + 7)) FAIL(31, "mapping lost data on mprotect\n");
    }

    // --- and it goes away again ---
    if (failed(SYS2(SYS_munmap, raw, kMapLen))) FAIL(40, "munmap failed\n");

    // A second mapping should be able to reuse the freed address.
    const long again = sys6(SYS_mmap, 0, kMapLen, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (failed(again)) FAIL(41, "mmap after munmap failed\n");
    *(volatile unsigned char*)(ulong)again = 0xab;
    if (*(volatile unsigned char*)(ulong)again != 0xab) FAIL(42, "reused mapping is broken\n");

    FAIL(0, "memtest ok\n");
}
