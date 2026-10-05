#include "sentry.h"
#include "session.h"
#include "vfs_metadata.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>

#include <sys/utsname.h>
#include <sys/mman.h>
#include <unistd.h>

#include "guest_layout.h"

namespace goblin {

namespace {

// aarch64 syscall numbers. Only the ones worth naming in a trace -- an unnamed
// number is a fine trace entry, a wrong name is not.
struct NamedSyscall {
    long nr;
    const char* name;
};

constexpr NamedSyscall kSyscallNames[] = {
    {17, "getcwd"},         {19, "eventfd2"},     {23, "dup"},          {25, "fcntl"},
    {29, "ioctl"},          {48, "faccessat"},    {49, "chdir"},
    {56, "openat"},         {57, "close"},        {59, "pipe2"},
    {61, "getdents64"},     {62, "lseek"},        {63, "read"},
    {64, "write"},          {65, "readv"},        {66, "writev"},
    {67, "pread64"},        {68, "pwrite64"},     {78, "readlinkat"},
    {79, "newfstatat"},     {80, "fstat"},        {93, "exit"},
    {94, "exit_group"},     {96, "set_tid_address"}, {98, "futex"},
    {99, "set_robust_list"},{113, "clock_gettime"},  {115, "clock_nanosleep"},
    {124, "sched_yield"},   {129, "kill"},        {131, "tgkill"},
    {134, "rt_sigaction"},  {135, "rt_sigprocmask"}, {139, "rt_sigreturn"},
    {160, "uname"},         {172, "getpid"},      {173, "getppid"},
    {174, "getuid"},        {175, "geteuid"},     {176, "getgid"},
    {177, "getegid"},       {178, "gettid"},      {214, "brk"},
    {215, "munmap"},        {216, "mremap"},      {220, "clone"},       {221, "execve"},
    {222, "mmap"},          {226, "mprotect"},    {233, "madvise"},
    {260, "wait4"},         {261, "prlimit64"},   {278, "getrandom"},
    {279, "memfd_create"},  {293, "rseq"},
    {436, "close_range"},
};

enum : long {
    kWrite = 64,
    kWritev = 66,
    kExit = 93,
    kExitGroup = 94,
    kSetTidAddress = 96,
    kUname = 160,
    kBrk = 214,
    kMunmap = 215,
    kMmap = 222,
    kMprotect = 226,
    kMadvise = 233,
    kGetpid = 172,
    kGetppid = 173,
    kGetuid = 174,
    kGeteuid = 175,
    kGetgid = 176,
    kGetegid = 177,
    kGettid = 178,
};

// The guest's view of itself. Deliberately not Android's: the guest is a Linux
// machine, and telling it otherwise would be the first of many lies that break
// software expecting a normal distribution.
constexpr char kSysname[] = "Linux";
constexpr char kNodename[] = "goblin";
constexpr char kRelease[] = "6.6.0-goblin";
constexpr char kVersion[] = "#1 SMP goblin-linux";
constexpr char kMachine[] = "aarch64";

// Guest pids are the sentry's to assign; host pids must never leak through.

}  // namespace

const char* SyscallName(long nr) {
    for (const NamedSyscall& s : kSyscallNames) {
        if (s.nr == nr) return s.name;
    }
    return nullptr;
}

long Sentry::SysUname(uintptr_t buf) {
    utsname u{};
    snprintf(u.sysname, sizeof(u.sysname), "%s", kSysname);
    snprintf(u.nodename, sizeof(u.nodename), "%s", kNodename);
    snprintf(u.release, sizeof(u.release), "%s", kRelease);
    snprintf(u.version, sizeof(u.version), "%s", kVersion);
    snprintf(u.machine, sizeof(u.machine), "%s", kMachine);
    if (!mem_.Write(buf, &u, sizeof(u))) return -EFAULT;
    return 0;
}

void Sentry::Record(const SyscallRequest& req, long ret) {
    ++syscall_count_;
    const bool failed = ret < 0 && ret > -4096;
    if (trace_.size() >= kMaxTrace && !failed) return;

    // Addresses in decimal are unreadable, and most of what comes back from a
    // memory syscall is an address. Errors stay decimal so -ENOSYS reads as -38
    // rather than as a very large pointer.
    char result[32];
    if (ret < 0 && ret > -4096) {
        snprintf(result, sizeof(result), "%ld", ret);
    } else if (ret > 0xffff) {
        snprintf(result, sizeof(result), "%#lx", static_cast<unsigned long>(ret));
    } else {
        snprintf(result, sizeof(result), "%ld", ret);
    }

    char buf[256];
    const char* name = SyscallName(req.nr);
    char nr_text[24];
    if (name == nullptr) {
        snprintf(nr_text, sizeof(nr_text), "syscall_%ld", req.nr);
        name = nr_text;
    }
    snprintf(buf, sizeof(buf), "%-16s(%#lx, %#lx, %#lx) = %s", name, req.args[0],
             req.args[1], req.args[2], result);
    if (trace_.size() < kMaxTrace) trace_.emplace_back(buf);
    if (failed) {
        recent_errors_.emplace_back(buf);
        if (recent_errors_.size() > 96) recent_errors_.pop_front();
    }
}

long Sentry::Handle(const SyscallRequest& req, AddressSpace::StubOp* op) {
    CredentialScope credentials_scope(credentials);
    *op = AddressSpace::StubOp{};
    if (pending_space_) return -EBUSY;
    const bool memory = req.nr == kBrk || req.nr == kMmap ||
        req.nr == kMunmap || req.nr == kMprotect || req.nr == kMadvise || req.nr == 227 || req.nr == 216;
    std::optional<AddressSpace> proposed;
    if (memory) proposed = *space_;
    long ret;
    std::optional<long> external;
    files_->lock_owner = tgid;
    files_->current_tid = pid;
    auto handler = dispatch; // exec may replace this Sentry during the call.
    external = credentials.Handle(req, mem_);
    if (!external && handler) external = handler(req);
    if (!external) external = files_->Handle(req, mem_);
    if (external) {
        if (*external != kBlocked) Record(req, *external == LONG_MIN + 1 ? 0 : *external);
        return *external;
    }
    switch (req.nr) {
        // This filesystem does not expose guest xattrs. In particular, its
        // broker-owned inode/mode/socket metadata must never reach the guest.
        // ENOTSUP lets copy2/dpkg preserve other metadata on such a filesystem.
        case 5: case 6: case 7: case 8: case 9: case 10:
        case 11: case 12: case 13: case 14: case 15: case 16:
            ret = -EOPNOTSUPP; break;
        case kExit:
        case kExitGroup:
            exited_ = true;
            exit_status_ = static_cast<int>(req.args[0]) & 0xff;
            ret = 0;
            break;
        case kBrk:
            ret = proposed->Brk(req.args[0], op);
            break;
        case kMmap:
            if (req.args[5] % static_cast<unsigned long>(sysconf(_SC_PAGESIZE)) != 0) {
                ret = -EINVAL;
            } else if ((req.args[3] & MAP_ANONYMOUS) == 0) {
                const auto* descriptor = files_->Get(req.args[4]);
                const int fd = files_->HostFd(req.args[4]);
                if (!descriptor || fd < 0) { ret = -EBADF; break; }
                if ((descriptor->file->flags & O_ACCMODE) == O_WRONLY ||
                    (descriptor->file->flags & O_PATH)) { ret = -EACCES; break; }
                if ((req.args[3] & MAP_TYPE) == MAP_SHARED && (req.args[2] & PROT_WRITE) &&
                    (descriptor->file->flags & O_ACCMODE) != O_RDWR) { ret = -EACCES; break; }
                if (req.args[1] > (64u << 20) || req.args[5] > INT64_MAX - req.args[1]) {
                    ret = -ENOMEM; break;
                }
                struct stat st{};
                if (fstat(fd, &st) < 0) { ret = -errno; break; }
                if (!S_ISREG(st.st_mode)) { ret = -ENODEV; break; }
                // A writable shared mapping can modify the inode after mmap,
                // including after a later mprotect adds write permission.
                if ((req.args[3] & MAP_TYPE) == MAP_SHARED && (descriptor->file->flags & O_ACCMODE) == O_RDWR) {
                    ret = ClearWritePrivilegeBits(fd); if (ret < 0) break;
                }
                ret = proposed->Mmap(req.args[0], req.args[1], req.args[2],
                                     req.args[3] | MAP_ANONYMOUS, op);
                if (ret < 0) break;
                op->kind = AddressSpace::StubOp::kMapFile;
                op->flags &= ~MAP_ANONYMOUS;
                op->fd = fd; op->offset = req.args[5];
                proposed->FileBacking(op->addr, op->len, descriptor->file->host, req.args[5]);
            } else {
                ret = proposed->Mmap(req.args[0], req.args[1], req.args[2], req.args[3], op);
                if (ret >= 0 && (req.args[3] & MAP_TYPE) == MAP_SHARED && shared_memory) {
                    long offset = shared_memory->Allocate(op->len);
                    if (offset < 0) { ret = offset; *op = {}; break; }
                    op->flags &= ~MAP_ANONYMOUS;
                    op->fd = shared_memory->file->fd; op->offset = offset;
                    proposed->SharedBacking(op->addr, op->len, offset);
                }
            }
            break;
        case 227:
            ret = proposed->Msync(req.args[0], req.args[1], req.args[2], op);
            break;
        case 216:
            ret = proposed->Mremap(req.args[0], req.args[1], req.args[2], req.args[3], op);
            break;
        case kMunmap:
            ret = proposed->Munmap(req.args[0], static_cast<size_t>(req.args[1]), op);
            break;
        case kMprotect:
            ret = proposed->Mprotect(req.args[0], static_cast<size_t>(req.args[1]),
                                  static_cast<int>(req.args[2]), op);
            break;
        case kMadvise:
            ret = proposed->Madvise(req.args[0], static_cast<size_t>(req.args[1]),
                                    static_cast<int>(req.args[2]), op);
            break;
        case kUname:
            ret = SysUname(req.args[0]);
            break;
        case kGetpid:
            ret = tgid;
            break;
        case kGetppid:
            ret = ppid;
            break;
        case kGettid:
            ret = pid;
            break;
        case kGetuid:
        case kGeteuid:
        case kGetgid:
        case kGetegid:
            // The guest is root inside its own world and nowhere else.
            ret = 0;
            break;
        case kSetTidAddress:
            clear_tid = req.args[0];
            ret = pid;
            break;
        case 99: // Task-owned robust lists are handled by Kernel::Dispatch.
            ret = req.args[1] == 24 ? -ENOSYS : -EINVAL;
            break;
        case 98: { // futex: uncontended libc locks and single-task waits.
            const unsigned command = req.args[1] & 127;
            uint32_t value;
            if (req.args[0] & 3) ret = -EINVAL;
            else if (!mem_.Read(req.args[0], &value, sizeof(value))) ret = -EFAULT;
            else if (command == 1 || command == 10) ret = 0;
            else if (command == 0 || command == 9) ret = value != req.args[2] ? -EAGAIN : -ENOSYS;
            else ret = -ENOSYS;
            break;
        }
        case 113: { // clock_gettime
            timespec ts{};
            if (req.args[0] != CLOCK_REALTIME && req.args[0] != CLOCK_MONOTONIC &&
                req.args[0] != CLOCK_MONOTONIC_RAW && req.args[0] != CLOCK_BOOTTIME &&
                req.args[0] != CLOCK_REALTIME_COARSE && req.args[0] != CLOCK_MONOTONIC_COARSE) ret = -EINVAL;
            else if (clock_gettime(req.args[0], &ts) < 0) ret = -errno;
            else ret = mem_.Write(req.args[1], &ts, sizeof(ts)) ? 0 : -EFAULT;
            break;
        }
        case 169: { // gettimeofday
            timeval tv{}; gettimeofday(&tv, nullptr);
            int zone[2] = {};
            ret = (req.args[0] && !mem_.Write(req.args[0], &tv, sizeof(tv))) ||
                (req.args[1] && !mem_.Write(req.args[1], zone, sizeof(zone))) ? -EFAULT : 0;
            break;
        }
        case 124: ret = 0; break; // sched_yield
        case 123: { // sched_getaffinity
            uint64_t cpus = 1;
            if (req.args[0] && req.args[0] != static_cast<unsigned>(pid)) ret = -ESRCH;
            else if (req.args[1] < 8) ret = -EINVAL;
            else ret = mem_.Write(req.args[2], &cpus, sizeof(cpus)) ? 8 : -EFAULT;
            break;
        }
        case 134: { // Linux kernel sigaction, 8-byte mask.
            const auto sig = req.args[0];
            if (!sig || sig > 64 || sig == SIGKILL || sig == SIGSTOP || req.args[3] != 8) { ret = -EINVAL; break; }
            SignalAction action{};
            if (req.args[1] && !mem_.Read(req.args[1], &action, sizeof(action))) { ret = -EFAULT; break; }
            if (req.args[2] && !mem_.Write(req.args[2], &actions()[sig], sizeof(action))) { ret = -EFAULT; break; }
            if (req.args[1]) actions()[sig] = action;
            ret = 0; break;
        }
        case 135: { // rt_sigprocmask
            uint64_t mask = 0;
            if (req.args[3] != 8 || (req.args[0] > 2 && req.args[1])) { ret = -EINVAL; break; }
            if (req.args[1] && !mem_.Read(req.args[1], &mask, 8)) { ret = -EFAULT; break; }
            if (req.args[2] && !mem_.Write(req.args[2], &signal_mask, 8)) { ret = -EFAULT; break; }
            mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
            if (req.args[1]) {
                if (req.args[0] == SIG_BLOCK) signal_mask |= mask;
                else if (req.args[0] == SIG_UNBLOCK) signal_mask &= ~mask;
                else signal_mask = mask;
            }
            ret = 0; break;
        }
        case 140: case 141: { // setpriority/getpriority: the guest scheduler uses nice 0.
            if (req.args[0] != PRIO_PROCESS) { ret = -EOPNOTSUPP; break; }
            if (req.args[1] && req.args[1] != static_cast<unsigned>(pid)) { ret = -ESRCH; break; }
            ret = req.nr == 141 ? 20 : static_cast<int>(req.args[2]) == 0 ? 0 : -EOPNOTSUPP;
            break;
        }
        case 261: { // prlimit64, guest limits do not alter the broker's limits.
            struct Limit { uint64_t current, maximum; } value{UINT64_MAX, UINT64_MAX}, change{};
            if (req.args[0] && req.args[0] != static_cast<unsigned>(pid)) { ret = -ESRCH; break; }
            switch (req.args[1]) {
                case RLIMIT_STACK: value = {GuestWindow::kStackSize, GuestWindow::kStackSize}; break;
                case RLIMIT_NOFILE: value = {256, 256}; break;
                case RLIMIT_NPROC: value.current = value.maximum = limits ? limits->tasks : 64; break;
                case RLIMIT_AS: case RLIMIT_DATA: value.current = value.maximum = limits ? limits->memory_bytes : GuestWindow::kSize; break;
                case RLIMIT_FSIZE: value.current = value.maximum = limits ? limits->disk_bytes : UINT64_MAX; break;
                case RLIMIT_CORE: value = {0, 0}; break;
                default: if (req.args[1] >= RLIM_NLIMITS) { ret = -EINVAL; } else ret = 0;
                    if (ret < 0) break;
            }
            if (req.args[1] >= RLIM_NLIMITS) { ret = -EINVAL; break; }
            if (req.args[2] && !mem_.Read(req.args[2], &change, sizeof(change))) { ret = -EFAULT; break; }
            if (req.args[2] && change.current > change.maximum) { ret = -EINVAL; break; }
            if (req.args[2] && change.maximum > value.maximum) { ret = -EPERM; break; }
            if (req.args[2] && (change.current != value.current || change.maximum != value.maximum)) { ret = -EOPNOTSUPP; break; }
            ret = req.args[3] && !mem_.Write(req.args[3], &value, sizeof(value)) ? -EFAULT : 0;
            break;
        }
        case 278: { // getrandom
            if (req.args[2] & ~3UL) { ret = -EINVAL; break; }
            std::vector<uint8_t> bytes(std::min<unsigned long>(req.args[1], 1u << 20));
            long n = syscall(__NR_getrandom, bytes.data(), bytes.size(), req.args[2]);
            ret = n < 0 ? -errno : !mem_.Write(req.args[0], bytes.data(), n) ? -EFAULT : n;
            break;
        }
        default:
            ret = -ENOSYS;
            break;
    }
    if (memory && ret >= 0 && allow_memory && !allow_memory(*proposed)) {
        ret = req.nr == kBrk ? static_cast<long>(space_->brk()) : -ENOMEM;
        *op = {}; proposed = *space_;
    }
    if (op->kind != AddressSpace::StubOp::kNone) {
        pending_space_ = std::move(proposed);
        pending_request_ = req;
        pending_return_ = ret;
        pending_op_ = *op;
    } else {
        if (memory && ret >= 0) *space_ = std::move(*proposed);
        Record(req, ret);
    }
    return ret;
}

long Sentry::CompleteMemoryOp(long result, AddressSpace::StubOp* next) {
    if (next) *next = {};
    if (!pending_space_) return -EPROTO;
    // mmap must have returned the exact address requested with MAP_FIXED.
    const long expected = (pending_op_.kind == AddressSpace::StubOp::kMap || pending_op_.kind == AddressSpace::StubOp::kMapFile)
        ? static_cast<long>(pending_op_.addr) : pending_op_.kind == AddressSpace::StubOp::kRemap
        ? static_cast<long>(pending_op_.new_addr) : 0;
    if (result >= 0 && result != expected) result = -EIO;
    long ret;
    if (result < 0) {
        ret = pending_request_.nr == kBrk ? static_cast<long>(space_->brk()) : result;
    } else {
        *space_ = std::move(*pending_space_);
        ret = pending_return_;
    }
    pending_space_.reset();
    Record(pending_request_, ret);
    return ret;
}

}  // namespace goblin

namespace goblin {
std::unique_ptr<Sentry> Sentry::ForkState(bool reset_trace) const {
    auto child = std::make_unique<Sentry>(*this);
    child->dispatch = {};
    child->space_ = std::make_shared<AddressSpace>(*space_);
    child->files_ = std::make_shared<FileTable>(*files_);
    child->actions_ = std::make_shared<std::array<SignalAction, 65>>(*actions_);
    if (reset_trace) { child->trace_.clear(); child->recent_errors_.clear(); child->syscall_count_ = 0; }
    child->clear_tid = 0;
    return child;
}
void Sentry::ResetAfterExec() {
    clear_tid = 0;
    for (auto& action : actions()) if (action.handler != 1) action = {};
    files_->lock_owner = tgid;
    files_->CloseExec();
    pending_space_.reset();
    exited_ = false; exit_status_ = 0;
}
}
