#include "stub_filter.h"
#include "shared_memory.h"
#include "guest_layout.h"
#include <cerrno>
#include <functional>
#include <linux/audit.h>
#include <linux/futex.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/socket.h>

namespace goblin {
namespace {
class Filter {
public:
    std::vector<sock_filter> code;
    void stmt(uint16_t op, uint32_t value) { code.push_back(BPF_STMT(op, value)); }
    void jump(uint16_t op, uint32_t value, uint8_t yes, uint8_t no) {
        code.push_back(BPF_JUMP(op, value, yes, no));
    }
    void load(size_t offset) { stmt(BPF_LD | BPF_W | BPF_ABS, offset); }
    void equal(uint32_t value, uint32_t failure = SECCOMP_RET_ERRNO | EPERM) {
        jump(BPF_JMP | BPF_JEQ | BPF_K, value, 1, 0);
        stmt(BPF_RET | BPF_K, failure);
    }
    void word(int arg, bool high = false) {
        load(offsetof(seccomp_data, args) + arg * sizeof(uint64_t) + (high ? 4 : 0));
    }
    void arg64(int arg, uint64_t value) {
        word(arg, true); equal(value >> 32);
        word(arg); equal(value);
    }
    void one_of(std::initializer_list<uint32_t> values) {
        size_t remaining = values.size();
        for (uint32_t value : values) {
            jump(BPF_JMP | BPF_JEQ | BPF_K, value, remaining--, 0);
        }
        stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    }
    void rule(long nr, const std::function<void()>& checks) {
        load(offsetof(seccomp_data, nr));
        const size_t branch = code.size();
        jump(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 0);
        checks();
        stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
        code[branch].jf = static_cast<uint8_t>(code.size() - branch - 1);
    }
};
}

std::vector<sock_filter> BuildStubFilter(uintptr_t gate_pc, uintptr_t guest_start,
                                        size_t guest_size, uintptr_t state_addr, int shared_fd, int transfer_fd) {
    Filter f;
    f.load(offsetof(seccomp_data, arch));
    f.equal(AUDIT_ARCH_AARCH64, SECCOMP_RET_KILL_PROCESS);
    f.load(offsetof(seccomp_data, instruction_pointer) + 4);
    f.equal(gate_pc >> 32, SECCOMP_RET_TRAP);
    f.load(offsetof(seccomp_data, instruction_pointer));
    f.equal(gate_pc, SECCOMP_RET_TRAP);

    auto range_args = [&](int address, int length) {
        f.word(length, true); f.equal(0);
        f.word(address, true);
        const size_t low = f.code.size();
        f.jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 0);
        auto bounds = [&](uint32_t begin, uint32_t end) {
            // Each range fits within one high word. Bounding both operands
            // before addition prevents wraparound and crossing the gap.
            f.word(address);
            f.jump(BPF_JMP | BPF_JGE | BPF_K, begin, 1, 0);
            f.stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
            f.jump(BPF_JMP | BPF_JGE | BPF_K, end, 0, 1);
            f.stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
            f.stmt(BPF_ST, 0);
            f.word(length);
            f.jump(BPF_JMP | BPF_JGT | BPF_K, end - begin, 0, 1);
            f.stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
            f.stmt(BPF_LDX | BPF_MEM, 0);
            f.stmt(BPF_ALU | BPF_ADD | BPF_X, 0);
            f.jump(BPF_JMP | BPF_JGT | BPF_K, end, 0, 1);
            f.stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
        };
        f.equal(guest_start >> 32); bounds(0, guest_size);
        const size_t done = f.code.size(); f.stmt(BPF_JMP | BPF_JA, 0);
        f.code[low].jt = f.code.size() - low - 1;
        bounds(GuestWindow::kLowStart, GuestWindow::kLowEnd);
        f.code[done].k = f.code.size() - done - 1;
    };
    auto range = [&] { range_args(0, 1); };
    auto protection = [&] {
        f.word(2, true); f.equal(0);
        f.word(2);
        f.stmt(BPF_ALU | BPF_AND | BPF_K, ~(PROT_READ | PROT_WRITE | PROT_EXEC));
        f.equal(0);
    };
    // A read-only clock query needs no broker state. The extra, kernel-ignored
    // argument fixes the output size so the same range checks protect the
    // runtime/channel even when a guest jumps directly to this gate.
    f.rule(__NR_gettimeofday, [&] {
        f.arg64(1, 0); f.arg64(2, 16); range_args(0, 2);
    });
    f.rule(__NR_mmap, [&] {
        range(); protection();
        if (transfer_fd >= 0) {
            f.word(4);
            const size_t other = f.code.size();
            f.jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 0);
            f.arg64(4, 0);
            f.word(3, true); f.equal(0);
            f.word(3); f.one_of({MAP_SHARED | MAP_FIXED, MAP_PRIVATE | MAP_FIXED});
            f.stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
            f.code[other].jf = f.code.size() - other - 1;
        }
        if (shared_fd >= 0) {
            f.word(4, true);
            const size_t anonymous = f.code.size();
            f.jump(BPF_JMP | BPF_JEQ | BPF_K, UINT32_MAX, 0, 0);
            f.arg64(4, shared_fd);
            f.arg64(3, MAP_SHARED | MAP_FIXED);
            f.word(5, true); f.equal(0);
            f.word(5);
            f.jump(BPF_JMP | BPF_JGT | BPF_K, SharedMemory::kLimit, 0, 1);
            f.stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
            f.stmt(BPF_ST, 1);
            f.word(1);
            f.stmt(BPF_LDX | BPF_MEM, 1);
            f.stmt(BPF_ALU | BPF_ADD | BPF_X, 0);
            f.jump(BPF_JMP | BPF_JGT | BPF_K, SharedMemory::kLimit, 0, 1);
            f.stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
            f.stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
            f.code[anonymous].jt = f.code.size() - anonymous - 1;
        }
        f.word(3, true); f.equal(0);
        f.word(3);
        f.one_of({MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE,
                  MAP_FIXED | MAP_ANONYMOUS | MAP_SHARED});
        f.arg64(4, UINT64_MAX);
        f.arg64(5, 0);
    });
    f.rule(__NR_munmap, range);
    f.rule(__NR_mremap, [&] {
        range_args(0, 1);
        f.word(3, true); f.equal(0);
        f.word(3);
        const size_t fixed = f.code.size();
        f.jump(BPF_JMP | BPF_JEQ | BPF_K, MREMAP_MAYMOVE | MREMAP_FIXED, 0, 0);
        f.equal(0); range_args(0, 2);
        f.stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
        f.code[fixed].jt = f.code.size() - fixed - 1;
        range_args(4, 2);
    });
    f.rule(__NR_mprotect, [&] { range(); protection(); });
    f.rule(__NR_msync, [&] {
        range(); f.word(2, true); f.equal(0);
        f.word(2); f.stmt(BPF_ALU | BPF_AND | BPF_K, ~(MS_SYNC | MS_ASYNC | MS_INVALIDATE)); f.equal(0);
    });
    if (transfer_fd >= 0) {
        // The broker sends only guest-file mapping capabilities. No host I/O,
        // descriptor forwarding, or descriptor creation is available here.
        f.rule(__NR_recvmsg, [&] { f.arg64(0, transfer_fd); f.arg64(2, MSG_CMSG_CLOEXEC | MSG_DONTWAIT); });
        f.rule(__NR_close, [&] { f.arg64(0, 0); });
    }
    f.rule(__NR_madvise, [&] {
        range();
        f.word(2, true); f.equal(0);
        f.word(2);
        f.one_of({MADV_NORMAL, MADV_RANDOM, MADV_SEQUENTIAL, MADV_WILLNEED,
                  MADV_DONTNEED, MADV_FREE, MADV_DONTFORK, MADV_DOFORK});
    });
    f.rule(__NR_futex, [&] {
        f.arg64(0, state_addr);
        f.word(1, true); f.equal(0);
        f.word(1); f.one_of({FUTEX_WAIT, FUTEX_WAKE});
    });
    f.rule(__NR_rt_sigreturn, [] {});
    f.rule(__NR_exit, [] {});
    f.rule(__NR_exit_group, [] {});
    f.stmt(BPF_RET | BPF_K, SECCOMP_RET_TRAP);
    return f.code;
}
}
