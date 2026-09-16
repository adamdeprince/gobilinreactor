// The userspace kernel: what a guest syscall actually means.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace goblin {

// Guest memory access. In phase 1 the guest shares the sentry's address space,
// so these are bounds-checked casts. Phase 2 gives guests their own processes and
// swaps the implementation for process_vm_readv/writev without the callers
// noticing -- which is why nothing reaches into guest memory directly.
namespace guest_memory {
bool Read(uintptr_t addr, void* dst, size_t len);
bool Write(uintptr_t addr, const void* src, size_t len);
}  // namespace guest_memory

struct SyscallRequest {
    long nr = 0;
    unsigned long args[6] = {};
    uintptr_t pc = 0;
};

const char* SyscallName(long nr);

class Sentry {
public:
    using LogFn = std::function<void(const std::string&)>;

    explicit Sentry(LogFn log) : log_(std::move(log)) {}

    // Returns what the guest should see in x0. A negative value is -errno, the
    // same convention the kernel uses.
    long Handle(const SyscallRequest& req);

    bool exited() const { return exited_; }
    int exit_status() const { return exit_status_; }

    // Everything the guest wrote to fd 1 and 2.
    const std::string& guest_stdout() const { return stdout_; }
    const std::string& guest_stderr() const { return stderr_; }

    const std::vector<std::string>& trace() const { return trace_; }
    uint64_t syscall_count() const { return syscall_count_; }
    bool trace_truncated() const { return syscall_count_ > trace_.size(); }

    void set_brk(uintptr_t brk) { brk_ = brk; }

private:
    long SysWrite(int fd, uintptr_t buf, size_t count);
    long SysWritev(int fd, uintptr_t iov, int iovcnt);
    long SysBrk(uintptr_t addr);
    long SysUname(uintptr_t buf);

    void Record(const SyscallRequest& req, long ret);

    LogFn log_;
    bool exited_ = false;
    int exit_status_ = 0;
    uintptr_t brk_ = 0;
    std::string stdout_;
    std::string stderr_;
    std::vector<std::string> trace_;
    uint64_t syscall_count_ = 0;
    static constexpr size_t kMaxTrace = 64;
    // How much of each sink has already been emitted as whole lines.
    size_t stdout_emitted_ = 0;
    size_t stderr_emitted_ = 0;
};

}  // namespace goblin
