// The userspace kernel: what a guest syscall actually means.
#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <functional>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "address_space.h"
#include "guest_memory.h"
#include "files.h"
#include "credentials.h"
#include "shared_memory.h"

namespace goblin {
struct RunLimits;
class SessionControl;
class RuntimeControl;

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
    // `op` receives any mapping work the stub must carry out for this
    // decision to take effect; kNone when there is none.
    long Handle(const SyscallRequest& req, AddressSpace::StubOp* op);
    // Publish a proposed memory change only after the stub applied it.
    long CompleteMemoryOp(long result, AddressSpace::StubOp* next = nullptr);

    bool exited() const { return exited_; }
    int exit_status() const { return exit_status_; }

    // Everything the guest wrote to fd 1 and 2.
    const std::string& guest_stdout() const { return files_->output->out; }
    const std::string& guest_stderr() const { return files_->output->err; }

    const std::vector<std::string>& trace() const { return trace_; }
    const std::deque<std::string>& recent_errors() const { return recent_errors_; }
    uint64_t syscall_count() const { return syscall_count_; }
    bool trace_truncated() const { return syscall_count_ > trace_.size(); }

    AddressSpace& address_space() { return *space_; }
    const AddressSpace& address_space() const { return *space_; }
    uint64_t reserved_memory() const { return pending_space_ ? pending_space_->MappedBytes() : space_->MappedBytes(); }

    // Set once the stub exists; until then guest memory is this process's.
    void set_guest_pid(pid_t pid) { mem_.set_pid(pid); }
    GuestMemory& memory() { return mem_; }
    FileTable& files() { return *files_; }
    const FileTable& files() const { return *files_; }
    std::vector<std::string> arguments{"goblin-guest"};
    std::vector<std::string> environment{"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/root", "TERM=dumb", "LC_ALL=C"};
    Credentials credentials;
    int pid = 1, tgid = 1, ppid = 0, group = 1, session = 1;
    uintptr_t clear_tid = 0;
    struct SignalAction { uint64_t handler, flags, restorer, mask; };
    std::array<SignalAction, 65>& actions() { return *actions_; }
    const std::array<SignalAction, 65>& actions() const { return *actions_; }
    void ShareMemory(const Sentry& parent) { space_ = parent.space_; }
    void ShareThreadState(const Sentry& parent) { space_ = parent.space_; files_ = parent.files_; actions_ = parent.actions_; }
    void ReleaseFiles() { files_ = std::make_shared<FileTable>(*files_); files_->CloseAll(); }
    void UnshareFiles() { files_ = std::make_shared<FileTable>(*files_); }
    uint64_t signal_mask = 0;
    using Dispatch = std::function<std::optional<long>(const SyscallRequest&)>;
    Dispatch dispatch;
    std::shared_ptr<SharedMemory> shared_memory;
    std::shared_ptr<RunLimits> limits;
    SessionControl* control = nullptr;
    RuntimeControl* runtime = nullptr;
    std::function<bool(const AddressSpace&)> allow_memory;
    void RecordExternal(const SyscallRequest& req, long result) { Record(req, result); }
    void ResetAfterExec();
    std::unique_ptr<Sentry> ForkState(bool reset_trace = true) const;

private:
    long SysUname(uintptr_t buf);

    void Record(const SyscallRequest& req, long ret);

    LogFn log_;
    GuestMemory mem_;
    std::shared_ptr<FileTable> files_ = std::make_shared<FileTable>();
    std::shared_ptr<AddressSpace> space_ = std::make_shared<AddressSpace>();
    std::shared_ptr<std::array<SignalAction, 65>> actions_ = std::make_shared<std::array<SignalAction, 65>>();
    std::optional<AddressSpace> pending_space_;
    SyscallRequest pending_request_;
    AddressSpace::StubOp pending_op_;
    long pending_return_ = 0;
    bool exited_ = false;
    int exit_status_ = 0;
    std::vector<std::string> trace_;
    std::deque<std::string> recent_errors_;
    uint64_t syscall_count_ = 0;
    static constexpr size_t kMaxTrace = 512;
};

}  // namespace goblin
