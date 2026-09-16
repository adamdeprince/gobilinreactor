#include "stub.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <setjmp.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <linux/audit.h>
#include <linux/auxvec.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>

#include "guest_layout.h"

namespace goblin {
namespace {

// Everything the SIGSYS handler touches lives here rather than on the stack,
// because siglongjmp restores registers and any local modified after sigsetjmp
// would be indeterminate on the way back.
struct GuestContext {
    Sentry* sentry = nullptr;
    sigjmp_buf jmp;
    volatile uint64_t syscalls = 0;
    volatile int fault_sig = 0;
    volatile uintptr_t fault_addr = 0;
    volatile uintptr_t fault_pc = 0;
    volatile double t_enter = 0;
    volatile double t_exit = 0;
    // Time spent inside the handler, to separate the sentry's own dispatch
    // cost from what signal delivery costs before we get control.
    volatile double handler_ns = 0;
};

thread_local GuestContext* t_ctx = nullptr;

double NowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1e9 + static_cast<double>(ts.tv_nsec);
}

// This handler allocates, locks and logs, none of which is async-signal-safe in
// the general case. It is safe here for a specific reason: SIGSYS is synchronous
// and raised by guest code, and guest code never calls into the host allocator,
// so this thread cannot already hold a lock the handler needs. A truly
// asynchronous signal would not have that guarantee.
void SigsysHandler(int /*sig*/, siginfo_t* si, void* vctx) {
    auto* uc = static_cast<ucontext_t*>(vctx);
    GuestContext* ctx = t_ctx;
    if (ctx == nullptr) _exit(90);

    const double t0 = NowNs();

    SyscallRequest req;
    req.nr = si->si_syscall;
    for (int i = 0; i < 6; ++i) req.args[i] = uc->uc_mcontext.regs[i];
    req.pc = uc->uc_mcontext.pc;

    ctx->syscalls = ctx->syscalls + 1;
    const long ret = ctx->sentry->Handle(req);

    if (ctx->sentry->exited()) {
        ctx->t_exit = NowNs();
        siglongjmp(ctx->jmp, 1);
    }
    ctx->handler_ns = ctx->handler_ns + (NowNs() - t0);
    // On aarch64 the saved PC already points past the `svc` and the syscall was
    // aborted, so writing x0 is exactly "return this value to the guest".
    uc->uc_mcontext.regs[0] = static_cast<unsigned long>(ret);
}

// A guest that faults should be reported, not take the app down with it.
void FaultHandler(int sig, siginfo_t* si, void* vctx) {
    auto* uc = static_cast<ucontext_t*>(vctx);
    GuestContext* ctx = t_ctx;
    if (ctx != nullptr && GuestWindow::Contains(uc->uc_mcontext.pc)) {
        ctx->fault_sig = sig;
        ctx->fault_addr = reinterpret_cast<uintptr_t>(si->si_addr);
        ctx->fault_pc = uc->uc_mcontext.pc;
        ctx->t_exit = NowNs();
        siglongjmp(ctx->jmp, 2);
    }
    // Not the guest's doing. Restore the default disposition and let it through,
    // so a sentry bug still produces an honest crash.
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    sigaction(sig, &dfl, nullptr);
    raise(sig);
}

bool InstallHandlers(std::string* err) {
    static bool installed = false;
    if (installed) return true;

    // The guest may have exhausted its own stack by the time it traps, so the
    // handler runs on one of ours.
    stack_t ss{};
    ss.ss_size = static_cast<size_t>(SIGSTKSZ) * 8;
    ss.ss_sp = malloc(ss.ss_size);
    ss.ss_flags = 0;
    if (ss.ss_sp == nullptr || sigaltstack(&ss, nullptr) != 0) {
        *err = std::string("sigaltstack: ") + strerror(errno);
        return false;
    }

    struct sigaction sa {};
    sa.sa_sigaction = SigsysHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSYS, &sa, nullptr) != 0) {
        *err = std::string("sigaction(SIGSYS): ") + strerror(errno);
        return false;
    }

    struct sigaction fa {};
    fa.sa_sigaction = FaultHandler;
    fa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&fa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) {
        if (sigaction(sig, &fa, nullptr) != 0) {
            *err = std::string("sigaction: ") + strerror(errno);
            return false;
        }
    }
    installed = true;
    return true;
}

// Traps every syscall whose instruction pointer lies inside the guest window.
//
// Filtering on the instruction pointer rather than the syscall number is what
// makes a single-threaded sentry possible: the handler's own syscalls originate
// from the sentry's code, mapped far above the window by Android's linker, so
// they are allowed through and there is no recursion to break.
bool InstallFilter(std::string* err) {
    // The window is 4 GiB aligned, so its low word starts at zero and the only
    // low-half test needed is against its size.
    const uint32_t window_hi = GuestWindow::high_word();
    const uint32_t window_size = static_cast<uint32_t>(GuestWindow::kSize);

    sock_filter filter[] = {
        // 0: reject anything that is not the ABI we expect.
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 0, 5),
        // 2: high word of the instruction pointer selects the window.
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(seccomp_data, instruction_pointer) + 4),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, window_hi, 0, 3),
        // 4: low word, which is the offset into the window.
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(seccomp_data, instruction_pointer)),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, window_size, 1, 0),
        // 6: inside the window -- the guest's.
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
        // 7: ours.
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog prog{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])),
                    filter};

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        *err = std::string("PR_SET_NO_NEW_PRIVS: ") + strerror(errno);
        return false;
    }
    // No TSYNC: this thread only, so the rest of the app stays unfiltered.
    if (syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) != 0) {
        *err = std::string("seccomp: ") + strerror(errno);
        return false;
    }
    return true;
}

bool MapGuestStack(std::string* err) {
    void* want = reinterpret_cast<void*>(GuestWindow::stack_bottom());
    void* got = mmap(want, GuestWindow::kStackSize, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (got == MAP_FAILED) {
        *err = std::string("mapping guest stack: ") + strerror(errno);
        return false;
    }
    return true;
}

// The initial process stack Linux hands a freshly exec'd program: argc, argv,
// envp, then the auxiliary vector. Phase 1's guest reads none of it, but a real
// libc start-up reads all of it, and getting the shape right now is cheaper than
// debugging it later.
uintptr_t BuildInitialStack(const LoadedImage& img) {
    uintptr_t p = GuestWindow::stack_top();

    const unsigned char random_bytes[16] = {0x9e, 0x37, 0x79, 0xb9, 0x7f, 0x4a,
                                            0x7c, 0x15, 0xf3, 0x9c, 0xc0, 0x60,
                                            0x5c, 0xed, 0xc8, 0x34};
    p -= sizeof(random_bytes);
    const uintptr_t at_random = p;
    guest_memory::Write(p, random_bytes, sizeof(random_bytes));

    static const char kArgv0[] = "goblin-guest";
    p -= sizeof(kArgv0);
    const uintptr_t argv0 = p;
    guest_memory::Write(p, kArgv0, sizeof(kArgv0));

    p &= ~static_cast<uintptr_t>(15);

    const std::vector<uint64_t> words = {
        1, argv0, 0,               // argc, argv[0], argv terminator
        0,                         // envp terminator
        AT_PAGESZ,  static_cast<uint64_t>(sysconf(_SC_PAGESIZE)),
        AT_ENTRY,   img.entry,
        AT_PHDR,    0,
        AT_RANDOM,  at_random,
        AT_NULL,    0,
    };

    const size_t bytes = words.size() * sizeof(uint64_t);
    const uintptr_t sp = (p - bytes) & ~static_cast<uintptr_t>(15);
    guest_memory::Write(sp, words.data(), bytes);
    return sp;
}

// Hands control to the guest. `entry` and `sp` are pinned into scratch registers
// first: once sp moves, anything the compiler had spilled to the old stack is
// unreachable, so neither value may live in memory at that point.
[[noreturn]] void EnterGuest(uintptr_t entry, uintptr_t sp) {
    register uintptr_t x_entry __asm__("x16") = entry;
    register uintptr_t x_sp __asm__("x17") = sp;
    __asm__ volatile("mov sp, %[sp]\n\t"
                     "br  %[entry]\n\t"
                     :
                     : [sp] "r"(x_sp), [entry] "r"(x_entry)
                     : "memory");
    __builtin_unreachable();
}

}  // namespace

RunResult RunGuest(const LoadedImage& img, Sentry* sentry) {
    RunResult r;

    if (!MapGuestStack(&r.error)) return r;
    if (!InstallHandlers(&r.error)) return r;

    sentry->set_brk(img.brk);
    const uintptr_t sp = BuildInitialStack(img);

    static thread_local GuestContext ctx;
    ctx = GuestContext{};
    ctx.sentry = sentry;
    t_ctx = &ctx;

    if (sigsetjmp(ctx.jmp, 1) == 0) {
        if (!InstallFilter(&r.error)) {
            t_ctx = nullptr;
            return r;
        }
        ctx.t_enter = NowNs();
        EnterGuest(img.entry, sp);
    }
    t_ctx = nullptr;

    r.entered = true;
    r.syscalls = ctx.syscalls;
    if (ctx.syscalls > 0 && ctx.t_exit > ctx.t_enter) {
        const double n = static_cast<double>(ctx.syscalls);
        r.ns_per_syscall = (ctx.t_exit - ctx.t_enter) / n;
        r.ns_in_handler = ctx.handler_ns / n;
    }

    if (ctx.fault_sig != 0) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "guest faulted: %s at pc=%#lx accessing %#lx",
                 strsignal(ctx.fault_sig), ctx.fault_pc, ctx.fault_addr);
        r.error = buf;
        return r;
    }

    r.exited = sentry->exited();
    r.status = sentry->exit_status();
    return r;
}

}  // namespace goblin
