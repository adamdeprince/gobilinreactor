// This file is a separate executable image, not part of the Android library.
// It has no libc, allocator, TLS, unwinder, or dynamic relocations. Its
// retained descriptors provide guest shared pages and receive broker-approved
// guest-file mapping capabilities.
#include "stub_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <ucontext.h>

extern const struct StubBoot stub_boot __attribute__((visibility("hidden")));
extern long stub_syscall(long, long, long, long, long, long, long);
extern void stub_restore(void) __attribute__((visibility("hidden")));
extern void stub_resume_frame(void*) __attribute__((noreturn, visibility("hidden")));

#define CALL(n,a,b,c,d,e,f) stub_syscall((n),(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f))

void* memset(void* dst, int c, size_t n) {
    unsigned char* p = dst;
    for (size_t i = 0; i < n; ++i) p[i] = (unsigned char)c;
    return dst;
}
void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = dst;
    const unsigned char* s = src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

static struct StubShared* channel(void) {
    return (struct StubShared*)(uintptr_t)stub_boot.shared_start;
}

static void post(uint32_t state) {
    struct StubShared* s = channel();
    // Sequential consistency closes the store/load race with the sleeper's
    // announcement. The futex expected-value check closes the remaining race.
    __atomic_store_n(&s->state, state, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s->sleepers, __ATOMIC_SEQ_CST)) {
        CALL(__NR_futex, &s->state, FUTEX_WAKE, 1, 0, 0, 0);
    }
}

static void await_reply(void) {
    struct StubShared* s = channel();
    for (int i = 0; i < 20000; ++i) {
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == STUB_REPLY) return;
        __asm__ volatile("yield" ::: "memory");
    }
    for (;;) {
        __atomic_fetch_add(&s->sleepers, 1, __ATOMIC_SEQ_CST);
        uint32_t state = __atomic_load_n(&s->state, __ATOMIC_SEQ_CST);
        if (state != STUB_REPLY) {
            // EAGAIN/EINTR are raw negative error numbers. Never touch errno.
            CALL(__NR_futex, &s->state, FUTEX_WAIT, state, 0, 0, 0);
        }
        __atomic_fetch_sub(&s->sleepers, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == STUB_REPLY) return;
    }
}

static void die(int status) __attribute__((noreturn));
static void die(int status) {
    CALL(__NR_exit_group, status, 0, 0, 0, 0, 0);
    __builtin_trap();
}

static void failure(int error) __attribute__((noreturn));
static void failure(int error) {
    channel()->fault_sig = -error;
    channel()->fault_pc = 0;
    channel()->fault_addr = 0;
    post(STUB_FAULTED);
    die(92);
}

static long map_file(const struct StubShared* s) {
    char byte;
    struct iovec iov = {&byte, 1};
    union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    memset(&control, 0, sizeof(control));
    message.msg_iov = &iov; message.msg_iovlen = 1;
    message.msg_control = control.bytes; message.msg_controllen = sizeof(control.bytes);
    long n = CALL(__NR_recvmsg, stub_boot.transfer_fd, &message,
                  MSG_CMSG_CLOEXEC | MSG_DONTWAIT, 0, 0, 0);
    if (n < 0) return n;
    struct cmsghdr* cmsg = (void*)control.bytes;
    int fd = -1;
    if (message.msg_controllen >= CMSG_LEN(sizeof(int)) &&
        cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
        memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    if (fd != 0 || (message.msg_flags & MSG_CTRUNC)) return -EPROTO;
    long result = CALL(__NR_mmap, s->op_addr, s->op_len, s->op_prot,
                       s->op_flags, fd, s->op_offset);
    CALL(__NR_close, fd, 0, 0, 0, 0, 0);
    return result;
}

static void handler(int sig, siginfo_t* info, void* context) {
    struct StubShared* s = channel();
    ucontext_t* uc = context;
    if (sig != SIGUSR1 && (sig != SIGSYS || info->si_code != 1 /* SYS_SECCOMP */)) {
        s->fault_sig = sig;
        s->fault_code = info->si_code;
        s->fault_pc = uc->uc_mcontext.pc;
        s->fault_addr = (uintptr_t)info->si_addr;
    }
    const long nr = sig == SIGUSR1 ? -1 : sig == SIGSYS && info->si_code == 1 ? info->si_syscall : -2;
    if (nr == __NR_gettimeofday && uc->uc_mcontext.regs[1] == 0) {
        const uintptr_t out = uc->uc_mcontext.regs[0];
        const struct StubBoot* b = &stub_boot;
        if ((out >= b->guest_start && out <= b->guest_end - 16) ||
            (out >= b->guest_low_start && out <= b->guest_low_end - 16)) {
            // Avoid a scheduler/UI round trip for adjacent clock reads. Linux
            // validates the writable mapping; seccomp independently bounds the
            // complete timeval. Other pointer/timezone cases use the broker.
            uc->uc_mcontext.regs[0] = CALL(__NR_gettimeofday, out, 0, 16, 0, 0, 0);
            return;
        }
    }
    const int exit_status = (int)uc->uc_mcontext.regs[0] & 255;
    s->nr = nr;
    for (int i = 0; i < 6; ++i) s->args[i] = uc->uc_mcontext.regs[i];
    s->pc = uc->uc_mcontext.pc;
    s->context.machine = uc->uc_mcontext;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(s->context.tls));
    s->context_changed = 0;
    post(STUB_REQUEST);
    await_reply();

    while (s->op_kind != 0) {
        // The seccomp argument checks are the security boundary even if a
        // guest corrupts this shared message or jumps straight to the gate.
        long result;
        switch (s->op_kind) {
            case 1: result = CALL(__NR_mmap, s->op_addr, s->op_len,
                                 s->op_prot, s->op_flags, s->op_fd, s->op_offset); break;
            case 2: result = CALL(__NR_munmap, s->op_addr, s->op_len, 0, 0, 0, 0); break;
            case 3: result = CALL(__NR_mprotect, s->op_addr, s->op_len,
                                 s->op_prot, 0, 0, 0); break;
            case 4: result = CALL(__NR_madvise, s->op_addr, s->op_len,
                                 s->op_prot, 0, 0, 0); break;
            case 5: result = map_file(s); break;
            case 6: result = CALL(__NR_msync, s->op_addr, s->op_len, s->op_flags, 0, 0, 0); break;
            case 7: result = s->op_fd; break;
            case 8: result = CALL(__NR_mremap, s->op_addr, s->op_len, s->op_new_len,
                                 s->op_flags, s->op_new_addr, 0); break;
            default: result = -EINVAL; break;
        }
        s->applied_result = result;
        post(STUB_APPLIED);
        await_reply();
    }
    if ((nr == __NR_exit || nr == __NR_exit_group) && !s->context_changed) {
        s->exit_status = exit_status;
        post(STUB_EXITED);
        die(exit_status);
    }
    if (s->context_changed) {
        uc->uc_mcontext = s->context.machine;
        __asm__ volatile("msr tpidr_el0, %0" :: "r"(s->context.tls) : "memory");
    } else if (nr != -1) uc->uc_mcontext.regs[0] = s->ret;
}

struct Range { uintptr_t start, end; };

// Read the child's own map list after fork, while only this thread exists.
// Parsing completes before any mappings are removed, so proc iteration cannot
// skip entries as the address space changes underneath it.
static void discard_inherited_mappings(const struct StubBoot* b) {
    struct Range maps[4096];
    unsigned count = 0;
    char buf[4096];
    uintptr_t start = 0, end = 0;
    int field = 0;
    long fd = CALL(__NR_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY | O_CLOEXEC, 0, 0, 0);
    if (fd < 0) failure((int)-fd);
    for (;;) {
        long n = CALL(__NR_read, fd, buf, sizeof(buf), 0, 0, 0);
        if (n == -EINTR) continue;
        if (n < 0) failure((int)-n);
        if (n == 0) break;
        for (long i = 0; i < n; ++i) {
            unsigned char c = buf[i];
            if (c == '\n') {
                if (count == 4096 || end <= start) failure(E2BIG);
                maps[count++] = (struct Range){start, end};
                start = end = 0;
                field = 0;
            } else if (field < 2) {
                if (c == '-') { field = 1; continue; }
                if (c == ' ') { field = 2; continue; }
                unsigned digit = c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
                if (digit > 15) failure(EINVAL);
                if (field == 0) start = start * 16 + digit;
                else end = end * 16 + digit;
            }
        }
    }
    CALL(__NR_close, fd, 0, 0, 0, 0, 0);
    struct Range keep[4] = {
        {b->region_start, b->region_end},
        {b->guest_start, b->guest_end},
        {b->guest_low_shadow, b->guest_low_shadow + b->guest_low_end - b->guest_low_start},
        {b->shared_start, b->shared_start + b->shared_size}
    };
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            if (keep[j].start < keep[i].start) {
                struct Range tmp = keep[i]; keep[i] = keep[j]; keep[j] = tmp;
            }
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        uintptr_t cursor = maps[i].start, end = maps[i].end;
        for (int j = 0; j < 4 && cursor < end; ++j) {
            if (keep[j].end <= cursor || keep[j].start >= end) continue;
            if (cursor < keep[j].start) {
                long r = CALL(__NR_munmap, cursor, keep[j].start - cursor, 0, 0, 0, 0);
                if (r < 0) failure((int)-r);
            }
            cursor = keep[j].end;
        }
        if (cursor < end) {
            long r = CALL(__NR_munmap, cursor, end - cursor, 0, 0, 0, 0);
            if (r < 0) failure((int)-r);
        }
    }
    // Android can occupy the guest's fixed addresses in the parent. Its mappings
    // are now gone in this child. Move each staging VMA independently (mremap
    // cannot move a mixed-protection image as one span), preserving COW and files.
    const uintptr_t shadow_end = b->guest_low_shadow + b->guest_low_end - b->guest_low_start;
    for (unsigned i = 0; i < count; ++i) {
        uintptr_t lo = maps[i].start > b->guest_low_shadow ? maps[i].start : b->guest_low_shadow;
        uintptr_t hi = maps[i].end < shadow_end ? maps[i].end : shadow_end;
        if (hi <= lo) continue;
        uintptr_t target = b->guest_low_start + lo - b->guest_low_shadow;
        long r = CALL(__NR_mremap, lo, hi - lo, hi - lo, MREMAP_MAYMOVE | MREMAP_FIXED, target, 0);
        if (r < 0) failure((int)-r);
        if ((uintptr_t)r != target) failure(EFAULT);
    }
}

void stub_boot_main(const struct StubBoot* b) __attribute__((noreturn));
void stub_boot_main(const struct StubBoot* b) {
    channel()->boot_stage = 1;
    if (CALL(__NR_prctl, PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0, 0) < 0 ||
        CALL(__NR_getppid, 0, 0, 0, 0, 0, 0) != b->parent_pid) failure(ECHILD);
    discard_inherited_mappings(b);
    channel()->boot_stage = 2;
    // close_range may be absent or denied by the inherited Android filter.
    long r = 0;
    int low = b->shared_memory_fd, high = b->transfer_fd;
    if (low > high) { int tmp = low; low = high; high = tmp; }
    if (low < 0) low = high;
    if (low > 0) r = CALL(__NR_close_range, 0, low - 1, 0, 0, 0, 0);
    if (high > low + 1 && CALL(__NR_close_range, low + 1, high - 1, 0, 0, 0, 0) < 0) r = -1;
    if (CALL(__NR_close_range, high + 1, ~0u, 0, 0, 0, 0) < 0) r = -1;
    if (r < 0) {
        for (uint64_t fd = 0; fd < b->fd_limit; ++fd)
            if (fd != (uint64_t)b->shared_memory_fd && fd != (uint64_t)b->transfer_fd)
                CALL(__NR_close, fd, 0, 0, 0, 0, 0);
    }
    channel()->boot_stage = 3;
    stack_t stack = {.ss_sp = (void*)(uintptr_t)b->signal_stack,
                     .ss_flags = 0, .ss_size = b->signal_stack_size};
    r = CALL(__NR_sigaltstack, &stack, 0, 0, 0, 0, 0);
    if (r < 0) failure((int)-r);
    // Kernel sigaction layout, not bionic's expanded sigset_t layout.
    struct {
        void (*action)(int, siginfo_t*, void*);
        unsigned long flags;
        void (*restorer)(void);
        uint64_t mask;
    } action;
    // Form addresses at runtime. An aggregate initializer can be hoisted into
    // a table containing absolute link-time function addresses.
    action.action = handler;
    action.flags = SA_SIGINFO | SA_ONSTACK | 0x04000000UL;
    action.restorer = stub_restore;
    action.mask = ~0ULL;
    const int signals[] = {SIGSYS, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGUSR1};
    for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        r = CALL(__NR_rt_sigaction, signals[i], &action, 0, sizeof(uint64_t), 0, 0);
        if (r < 0) failure((int)-r);
    }
    // Notifications remain blocked until rt_sigreturn atomically restores the
    // guest registers and mask. They must never expose bootstrap registers as
    // a guest signal frame when another virtual process signals a new child.
    uint64_t mask = 1ULL << (SIGUSR1 - 1);
    channel()->boot_stage = 4;
    r = CALL(__NR_rt_sigprocmask, SIG_SETMASK, &mask, 0, sizeof(mask), 0, 0);
    if (r < 0) failure((int)-r);
    struct sock_fprog filter = {b->filter_size, (struct sock_filter*)b->filter};
    r = CALL(__NR_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0, 0);
    if (r < 0) failure((int)-r);
    r = CALL(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &filter, 0, 0, 0);
    if (r < 0) failure((int)-r);
    channel()->boot_stage = 5;
    struct { siginfo_t info; ucontext_t uc; } frame;
    memset(&frame, 0, sizeof(frame));
    frame.uc.uc_stack = stack;
    uintptr_t tls = 0;
    if (b->resume) {
        frame.uc.uc_mcontext = b->context.machine;
        tls = b->context.tls;
    } else {
        frame.uc.uc_mcontext.pc = b->guest_entry;
        frame.uc.uc_mcontext.sp = b->guest_sp;
        struct fpsimd_context* fp = (void*)frame.uc.uc_mcontext.__reserved;
        fp->head.magic = FPSIMD_MAGIC;
        fp->head.size = sizeof(*fp);
    }
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(tls) : "memory");
    stub_resume_frame(&frame);
}
