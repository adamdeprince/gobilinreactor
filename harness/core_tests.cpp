#include "core_tests.h"
#include <cerrno>
#include <cstring>
#include <linux/audit.h>
#include <linux/futex.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <unistd.h>
#include "address_space.h"
#include "guest_layout.h"
#include "sentry.h"
#include "stub_abi.h"
#include "stub_filter.h"

namespace {
// Execute the generated classic BPF against adversarial seccomp_data. These
// checks exercise the actual filter, rather than a duplicate policy function.
uint32_t Evaluate(const std::vector<sock_filter>& code, const seccomp_data& data) {
    uint32_t a = 0, x = 0, mem[16]{};
    for (size_t pc = 0, steps = 0; pc < code.size() && steps < 1024; ++pc, ++steps) {
        const auto& insn = code[pc];
        switch (insn.code) {
            case BPF_LD | BPF_W | BPF_ABS:
                if (insn.k > sizeof(data) - 4) return 0xffffffff;
                memcpy(&a, reinterpret_cast<const char*>(&data) + insn.k, 4);
                break;
            case BPF_ST:
                if (insn.k >= 16) return 0xffffffff;
                mem[insn.k] = a; break;
            case BPF_LDX | BPF_MEM:
                if (insn.k >= 16) return 0xffffffff;
                x = mem[insn.k]; break;
            case BPF_ALU | BPF_ADD | BPF_X: a += x; break;
            case BPF_ALU | BPF_AND | BPF_K: a &= insn.k; break;
            case BPF_JMP | BPF_JEQ | BPF_K: pc += a == insn.k ? insn.jt : insn.jf; break;
            case BPF_JMP | BPF_JGE | BPF_K: pc += a >= insn.k ? insn.jt : insn.jf; break;
            case BPF_JMP | BPF_JGT | BPF_K: pc += a > insn.k ? insn.jt : insn.jf; break;
            case BPF_JMP | BPF_JA: pc += insn.k; break;
            case BPF_RET | BPF_K: return insn.k;
            default: return 0xffffffff;
        }
    }
    return 0xffffffff;
}
}

bool RunCoreTests(const std::function<void(const std::string&)>& log) {
    using namespace goblin;
    bool ok = true;
    unsigned checks = 0;
    auto check = [&](bool pass, const char* what) {
        ++checks;
        if (!pass) { ok = false; log(std::string("FAILED core test: ") + what); }
    };
    const uintptr_t base = GuestWindow::pie_base();
    const size_t ps = sysconf(_SC_PAGESIZE);
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    AddressSpace as;
    AddressSpace::StubOp op;
    as.Init(base);
    check(as.Brk(base + 1, &op) == static_cast<long>(base + 1) && op.len == ps,
          "brk reports byte precision while mapping pages");
    check(as.Brk(base + 2, &op) == static_cast<long>(base + 2) && op.kind == AddressSpace::StubOp::kNone,
          "brk within the same page needs no host operation");
    check(as.Brk(base, &op) == static_cast<long>(base) && op.len == ps,
          "brk shrink releases the whole unused page");
    check(as.Mmap(0, SIZE_MAX, 3, flags, &op) == -ENOMEM, "mmap length overflow");
    check(as.Mmap(0, 0, 3, flags, &op) == -EINVAL, "zero mmap length");
    check(as.Mmap(base + 1, ps, 3, flags | MAP_FIXED, &op) == -EINVAL, "unaligned MAP_FIXED");
    check(as.Mmap(0, ps, 3, MAP_ANONYMOUS, &op) == -EINVAL, "missing map type");
    check(as.Mmap(0, ps, 3, flags | MAP_GROWSDOWN, &op) == -EINVAL, "unsafe grow-down flag");
    check(as.Mmap(UINTPTR_MAX - ps + 1, ps, 3, flags | MAP_FIXED, &op) == -ENOMEM,
          "fixed address arithmetic overflow");
    check(as.Mmap(base, ps, 3, flags | MAP_FIXED_NOREPLACE, &op) == static_cast<long>(base),
          "NOREPLACE creates at exact free address");
    check((op.flags & MAP_FIXED_NOREPLACE) == 0, "guest NOREPLACE removed from host operation");
    const std::string maps = as.DumpMaps();
    check(as.Mmap(base, ps, 1, flags | MAP_FIXED_NOREPLACE, &op) == -EEXIST && as.DumpMaps() == maps,
          "NOREPLACE collision preserves old mapping");
    check(as.Mprotect(base, ps, 0x40000000, &op) == -EINVAL, "invalid protection bits");
    check(as.Mprotect(base, SIZE_MAX, 1, &op) == -ENOMEM, "mprotect length overflow");
    check(as.Munmap(base, SIZE_MAX, &op) == -EINVAL, "munmap length overflow");
    check(as.Madvise(base, ps, MADV_DONTNEED, &op) == 0 && op.kind == AddressSpace::StubOp::kAdvise,
          "DONTNEED produces a real host operation");
    check(as.Madvise(base, ps, 0x7fffffff, &op) == -EINVAL, "unknown advice fails");

    Sentry sentry({});
    sentry.address_space().Init(base);
    SyscallRequest req;
    req.nr = __NR_mmap;
    req.args[1] = ps; req.args[2] = 3; req.args[3] = flags; req.args[4] = -1;
    long address = sentry.Handle(req, &op);
    check(address > 0 && sentry.address_space().vmas().empty(), "mapping remains pending until acknowledgement");
    check(sentry.CompleteMemoryOp(-ENOMEM) == -ENOMEM && sentry.address_space().vmas().empty(),
          "native mmap failure rolls back bookkeeping");
    check(sentry.CompleteMemoryOp(0) == -EPROTO, "unsolicited acknowledgement rejected");
    address = sentry.Handle(req, &op);
    check(sentry.CompleteMemoryOp(address) == address && sentry.address_space().vmas().size() == 1,
          "successful acknowledgement commits mapping");
    req.nr = __NR_mprotect; req.args[0] = address; req.args[2] = PROT_READ;
    sentry.Handle(req, &op);
    check(sentry.CompleteMemoryOp(-EACCES) == -EACCES && sentry.address_space().vmas()[0].prot == 3,
          "native mprotect failure preserves bookkeeping");
    req = {}; req.nr = __NR_mremap; req.args[0] = address; req.args[1] = ps;
    req.args[2] = 2 * ps; req.args[3] = MREMAP_MAYMOVE;
    const std::string before_remap = sentry.address_space().DumpMaps();
    long moved = sentry.Handle(req, &op);
    check(moved > 0 && moved != address && op.kind == AddressSpace::StubOp::kRemap &&
          sentry.address_space().DumpMaps() == before_remap, "mremap remains pending until acknowledgement");
    check(sentry.CompleteMemoryOp(-ENOMEM) == -ENOMEM && sentry.address_space().DumpMaps() == before_remap,
          "native mremap failure preserves old mapping");
    moved = sentry.Handle(req, &op);
    check(sentry.CompleteMemoryOp(moved) == moved && !sentry.address_space().Lookup(address) &&
          sentry.address_space().Lookup(moved)->end == uintptr_t(moved) + 2 * ps, "mremap acknowledgement publishes moved range");
    req.nr = __NR_brk; req.args[0] = base + 1;
    sentry.Handle(req, &op);
    check(sentry.CompleteMemoryOp(-ENOMEM) == static_cast<long>(base) && sentry.address_space().brk() == base,
          "failed brk growth returns old exact break");

    const uintptr_t gate_pc = GuestWindow::end() + 0x10030;
    const uintptr_t state = GuestWindow::end() + 0x30000;
    const auto filter = BuildStubFilter(gate_pc, GuestWindow::start(), GuestWindow::kSize, state);
    check(filter.size() <= STUB_MAX_FILTER, "filter fits boot configuration");
    seccomp_data data{};
    data.arch = AUDIT_ARCH_AARCH64; data.instruction_pointer = gate_pc;
    auto action = [&] { return Evaluate(filter, data); };
    for (int nr : {__NR_openat, __NR_read, __NR_write, __NR_ioctl, __NR_clone,
                   __NR_ptrace, __NR_process_vm_writev, __NR_prctl, __NR_seccomp}) {
        data.nr = nr;
        check(action() == SECCOMP_RET_TRAP, "gate cannot bypass syscall mediation");
    }
    data.nr = __NR_gettimeofday;
    data.args[0] = base; data.args[2] = 16;
    check(action() == SECCOMP_RET_ALLOW, "bounded time query allowed at native gate");
    data.args[0] = GuestWindow::end() - 16;
    check(action() == SECCOMP_RET_ALLOW, "time query at final guest bytes allowed");
    data.args[0] = GuestWindow::kLowEnd - 16;
    check(action() == SECCOMP_RET_ALLOW, "time query in low guest region allowed");
    for (uint64_t address : {uint64_t(0), uint64_t(GuestWindow::kLowEnd - 8),
                            uint64_t(GuestWindow::end() - 8), uint64_t(state), UINT64_MAX}) {
        data.args[0] = address;
        check(action() == (SECCOMP_RET_ERRNO | EPERM), "time query cannot overwrite runtime or cross guest bounds");
    }
    data.args[0] = base; data.args[1] = state;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "native time query cannot write a timezone pointer");
    data.args[1] = 0; data.args[2] = 8;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "native time query cannot understate its output size");
    data.args[2] = (1ull << 32) + 16;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "native time query size checks all bits");
    data.args[2] = 16; data.instruction_pointer = base;
    check(action() == SECCOMP_RET_TRAP, "guest time calls still enter the checked stub handler");
    data.instruction_pointer = gate_pc;
    data.nr = __NR_mmap;
    data.args[0] = base; data.args[1] = ps; data.args[2] = 3;
    data.args[3] = flags | MAP_FIXED; data.args[4] = UINT64_MAX;
    check(action() == SECCOMP_RET_ALLOW, "anonymous mapping inside window allowed");
    data.args[0] = GuestWindow::kLowStart;
    check(action() == SECCOMP_RET_ALLOW, "low ELF region mapping allowed");
    data.args[0] = 0;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "mapping below low region denied");
    data.args[0] = GuestWindow::kLowEnd;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "mapping past low guest region denied");
    data.args[0] = GuestWindow::kLowEnd - ps; data.args[1] = 2 * ps;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "mapping crossing low region end denied");
    data.args[1] = ps;
    data.args[0] = gate_pc;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "runtime overwrite denied");
    data.args[0] = GuestWindow::end() - ps; data.args[1] = 2 * ps;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "mapping crossing window end denied");
    data.args[0] = base; data.args[1] = UINT64_MAX;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "64-bit length overflow denied at gate");
    data.args[1] = ps; data.args[4] = 3;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "file descriptor mapping denied");
    data.args[4] = UINT64_MAX; data.args[3] |= MAP_GROWSDOWN;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "grow-down flag denied at gate");
    data.args[3] = flags | MAP_FIXED; data.args[2] = 0x100000003ULL;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "high protection bits denied");
    data.args[2] = 3; data.args[5] = 1;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "nonzero map offset denied at gate");
    data.nr = __NR_mremap; data.args[0] = base; data.args[1] = ps; data.args[2] = 2 * ps;
    data.args[3] = MREMAP_MAYMOVE | MREMAP_FIXED; data.args[4] = base + 4 * ps;
    check(action() == SECCOMP_RET_ALLOW, "mremap with both ranges inside window allowed");
    data.args[0] = GuestWindow::kLowStart;
    check(action() == SECCOMP_RET_ALLOW, "mremap may move low guest memory into high region");
    data.args[0] = GuestWindow::kLowEnd - ps; data.args[1] = 2 * ps;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "mremap cannot cross low guest region end");
    data.args[1] = ps;
    data.args[0] = state;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "mremap cannot move runtime or channel into guest");
    data.args[0] = base; data.args[4] = state;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "mremap cannot overwrite runtime or channel");
    data.args[4] = GuestWindow::end() - ps;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "mremap destination end bounded");
    data.args[4] = base + 4 * ps; data.args[1] = UINT64_MAX;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "mremap old size overflow denied");
    data.args[1] = ps; data.args[2] = UINT64_MAX;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "mremap new size overflow denied");
    data.args[2] = ps; data.args[3] = MREMAP_MAYMOVE;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "unconstrained native mremap placement denied");
    data.args[3] = 0; data.args[0] = GuestWindow::end() - ps; data.args[2] = 2 * ps;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "in-place mremap cannot grow past window");
    data.args[0] = base; data.args[2] = ps;
    check(action() == SECCOMP_RET_ALLOW, "in-place mremap inside window allowed");
    check(!GuestWindow::ContainsRange(GuestWindow::kLowEnd - ps, 2 * ps) &&
          !GuestWindow::ContainsRange(GuestWindow::kLowStart, GuestWindow::end() - GuestWindow::kLowStart),
          "guest copies cannot span the gap between regions");
    data.nr = __NR_futex; data.args[0] = state; data.args[1] = FUTEX_WAKE;
    check(action() == SECCOMP_RET_ALLOW, "channel futex wake allowed");
    data.args[0] = base;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "arbitrary futex address denied");
    data.args[0] = state; data.args[1] = FUTEX_REQUEUE;
    check(action() == (SECCOMP_RET_ERRNO | EPERM), "futex requeue denied");
    data.nr = __NR_rt_sigreturn;
    check(action() == SECCOMP_RET_ALLOW, "runtime signal return allowed");
    data.instruction_pointer = base;
    check(action() == SECCOMP_RET_TRAP, "guest signal return still mediated");
    data.instruction_pointer = gate_pc + 4;
    check(action() == SECCOMP_RET_TRAP, "other runtime instructions not implicitly trusted");
    data.arch = AUDIT_ARCH_ARM;
    check(action() == SECCOMP_RET_KILL_PROCESS, "unexpected architecture fails closed");
    const auto shared_filter = BuildStubFilter(gate_pc, GuestWindow::start(), GuestWindow::kSize, state, 42);
    check(shared_filter.size() <= STUB_MAX_FILTER, "shared backing filter fits boot configuration");
    data = {}; data.arch = AUDIT_ARCH_AARCH64; data.instruction_pointer = gate_pc;
    data.nr = __NR_mmap; data.args[0] = base; data.args[1] = ps; data.args[2] = 3;
    data.args[3] = MAP_FIXED | MAP_SHARED; data.args[4] = 42;
    auto shared_action = [&] { return Evaluate(shared_filter, data); };
    check(shared_action() == SECCOMP_RET_ALLOW, "only the dedicated guest memory descriptor can be mapped");
    data.args[4] = 43;
    check(shared_action() == (SECCOMP_RET_ERRNO | EPERM), "other retained descriptor numbers are rejected");
    data.args[4] = 42; data.args[5] = SharedMemory::kLimit - ps;
    check(shared_action() == SECCOMP_RET_ALLOW, "last shared backing page is accessible");
    data.args[1] = 2 * ps;
    check(shared_action() == (SECCOMP_RET_ERRNO | EPERM), "shared backing end is checked with length");
    data.args[1] = ps; data.args[5] = UINT64_MAX - ps + 1;
    check(shared_action() == (SECCOMP_RET_ERRNO | EPERM), "shared backing offset overflow is rejected");
    data.args[5] = 0; data.args[0] = state;
    check(shared_action() == (SECCOMP_RET_ERRNO | EPERM), "shared memory cannot overwrite the channel");
    data.nr = __NR_ftruncate; data.args[0] = 42;
    check(shared_action() == SECCOMP_RET_TRAP, "guest cannot resize the backing file through the gate");

    const auto file_filter = BuildStubFilter(gate_pc, GuestWindow::start(), GuestWindow::kSize, state, 42, 43);
    check(file_filter.size() <= STUB_MAX_FILTER, "file capability filter fits boot configuration");
    data = {}; data.arch = AUDIT_ARCH_AARCH64; data.instruction_pointer = gate_pc;
    data.nr = __NR_mmap; data.args[0] = base; data.args[1] = ps;
    data.args[2] = PROT_READ; data.args[3] = MAP_FIXED | MAP_PRIVATE; data.args[4] = 0;
    check(Evaluate(file_filter, data) == SECCOMP_RET_ALLOW, "received mapping capability is usable inside guest window");
    data.args[0] = state;
    check(Evaluate(file_filter, data) == (SECCOMP_RET_ERRNO | EPERM), "file capability cannot replace channel memory");
    data.nr = __NR_recvmsg; data.args[0] = 43; data.args[2] = MSG_CMSG_CLOEXEC | MSG_DONTWAIT;
    check(Evaluate(file_filter, data) == SECCOMP_RET_ALLOW, "only broker capability endpoint can receive descriptors");
    data.args[0] = 42;
    check(Evaluate(file_filter, data) == (SECCOMP_RET_ERRNO | EPERM), "other receive endpoints rejected");
    data.nr = __NR_sendmsg; data.args[0] = 43;
    check(Evaluate(file_filter, data) == SECCOMP_RET_TRAP, "capabilities cannot be forwarded by the stub gate");
    data.nr = __NR_close; data.args[0] = 42;
    check(Evaluate(file_filter, data) == (SECCOMP_RET_ERRNO | EPERM), "shared memory capability cannot be closed by gate");

    AddressSpace shared;
    shared.Init(base);
    check(shared.Mmap(base, 4 * ps, 3, MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, &op) == static_cast<long>(base), "shared VMA created");
    shared.SharedBacking(base, 4 * ps, 7 * ps);
    check(shared.Mprotect(base + ps, 2 * ps, PROT_READ, &op) == 0 &&
          shared.Lookup(base + 2 * ps)->backing_offset == 8 * ps, "protection split preserves shared offsets");
    check(shared.Munmap(base + ps, ps, &op) == 0 &&
          shared.Lookup(base + 2 * ps)->backing_offset == 9 * ps, "unmap split advances shared offset");
    check(shared.Madvise(base + 2 * ps, ps, MADV_DONTFORK, &op) == 0, "DONTFORK updates inheritance after acknowledgement");
    shared.AfterFork();
    check(!shared.Lookup(base + 2 * ps) && shared.Lookup(base + 3 * ps), "fork drops only DONTFORK pages");
    log("core regressions     " + std::to_string(checks) + (ok ? " checks passed" : " checks, failures above"));
    return ok;
}
