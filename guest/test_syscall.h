#pragma once
typedef unsigned long ulong;
static long sys6(long nr, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2),
                      "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
    return x0;
}
static __attribute__((noreturn)) void finish(int status, const char* text, ulong len) {
    sys6(64, 1, (long)text, len, 0, 0, 0);
    sys6(94, status, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
#define DONE(status, text) finish(status, text, sizeof(text) - 1)
#define CHECK(condition, code) do { if (!(condition)) DONE(code, "regression failed\n"); } while (0)
static ulong auxv(const ulong* stack, ulong key) {
    const ulong* p = stack + 1 + stack[0] + 1;
    while (*p) ++p;
    ++p;
    for (; p[0]; p += 2) if (p[0] == key) return p[1];
    return 0;
}
static long gate6(ulong gate, long nr, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    // Branch directly to the svc instruction, bypassing all runtime validation.
    __asm__ volatile("blr %7" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2),
                      "r"(x3), "r"(x4), "r"(x5), "r"(gate) : "x30", "memory", "cc");
    return x0;
}
