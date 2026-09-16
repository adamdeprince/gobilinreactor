#include "sentry.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <sys/utsname.h>

#include "guest_layout.h"

namespace goblin {

namespace guest_memory {

namespace {
bool InGuest(uintptr_t addr, size_t len) {
    if (len == 0) return true;
    if (!GuestWindow::Contains(addr)) return false;
    if (len - 1 > GuestWindow::end() - 1 - addr) return false;  // no wrap
    return true;
}
}  // namespace

bool Read(uintptr_t addr, void* dst, size_t len) {
    if (!InGuest(addr, len)) return false;
    memcpy(dst, reinterpret_cast<const void*>(addr), len);
    return true;
}

bool Write(uintptr_t addr, const void* src, size_t len) {
    if (!InGuest(addr, len)) return false;
    memcpy(reinterpret_cast<void*>(addr), src, len);
    return true;
}

}  // namespace guest_memory

namespace {

// aarch64 syscall numbers. Only the ones worth naming in a trace -- an unnamed
// number is a fine trace entry, a wrong name is not.
struct NamedSyscall {
    long nr;
    const char* name;
};

constexpr NamedSyscall kSyscallNames[] = {
    {17, "getcwd"},         {23, "dup"},          {25, "fcntl"},
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
    {215, "munmap"},        {220, "clone"},       {221, "execve"},
    {222, "mmap"},          {226, "mprotect"},    {233, "madvise"},
    {260, "wait4"},         {261, "prlimit64"},   {278, "getrandom"},
    {279, "memfd_create"},  {293, "rseq"},
};

enum : long {
    kWrite = 64,
    kWritev = 66,
    kExit = 93,
    kExitGroup = 94,
    kSetTidAddress = 96,
    kUname = 160,
    kGetpid = 172,
    kGetppid = 173,
    kGetuid = 174,
    kGeteuid = 175,
    kGetgid = 176,
    kGetegid = 177,
    kGettid = 178,
    kBrk = 214,
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
constexpr long kGuestPid = 1;
constexpr long kGuestPpid = 0;

struct GuestIovec {
    uint64_t base;
    uint64_t len;
};

}  // namespace

const char* SyscallName(long nr) {
    for (const NamedSyscall& s : kSyscallNames) {
        if (s.nr == nr) return s.name;
    }
    return nullptr;
}

long Sentry::SysWrite(int fd, uintptr_t buf, size_t count) {
    if (fd != 1 && fd != 2) return -EBADF;
    if (count == 0) return 0;
    if (count > (1u << 20)) count = 1u << 20;

    std::string data(count, '\0');
    if (!guest_memory::Read(buf, data.data(), count)) return -EFAULT;

    std::string& sink = fd == 1 ? stdout_ : stderr_;
    size_t& emitted = fd == 1 ? stdout_emitted_ : stderr_emitted_;
    sink += data;
    if (log_) {
        // Emit whole lines only, so the trace stays readable.
        const char* prefix = fd == 1 ? "guest stdout: " : "guest stderr: ";
        size_t nl;
        while ((nl = sink.find('\n', emitted)) != std::string::npos) {
            log_(prefix + sink.substr(emitted, nl - emitted));
            emitted = nl + 1;
        }
    }
    return static_cast<long>(count);
}

long Sentry::SysWritev(int fd, uintptr_t iov, int iovcnt) {
    if (iovcnt < 0 || iovcnt > 1024) return -EINVAL;
    long total = 0;
    for (int i = 0; i < iovcnt; ++i) {
        GuestIovec v{};
        if (!guest_memory::Read(iov + static_cast<uintptr_t>(i) * sizeof(v), &v,
                                sizeof(v))) {
            return -EFAULT;
        }
        if (v.len == 0) continue;
        long n = SysWrite(fd, static_cast<uintptr_t>(v.base),
                          static_cast<size_t>(v.len));
        if (n < 0) return total > 0 ? total : n;
        total += n;
    }
    return total;
}

long Sentry::SysBrk(uintptr_t addr) {
    // Enough to satisfy a libc asking where the heap starts. Growing it needs
    // real mapping, which arrives with mmap in phase 2.
    if (addr == 0 || addr < brk_) return static_cast<long>(brk_);
    return static_cast<long>(brk_);
}

long Sentry::SysUname(uintptr_t buf) {
    utsname u{};
    snprintf(u.sysname, sizeof(u.sysname), "%s", kSysname);
    snprintf(u.nodename, sizeof(u.nodename), "%s", kNodename);
    snprintf(u.release, sizeof(u.release), "%s", kRelease);
    snprintf(u.version, sizeof(u.version), "%s", kVersion);
    snprintf(u.machine, sizeof(u.machine), "%s", kMachine);
    if (!guest_memory::Write(buf, &u, sizeof(u))) return -EFAULT;
    return 0;
}

void Sentry::Record(const SyscallRequest& req, long ret) {
    ++syscall_count_;
    if (trace_.size() >= kMaxTrace) return;

    char buf[256];
    const char* name = SyscallName(req.nr);
    if (name != nullptr) {
        snprintf(buf, sizeof(buf), "%-16s(%#lx, %#lx, %#lx) = %ld", name,
                 req.args[0], req.args[1], req.args[2], ret);
    } else {
        snprintf(buf, sizeof(buf), "syscall_%-8ld(%#lx, %#lx, %#lx) = %ld",
                 req.nr, req.args[0], req.args[1], req.args[2], ret);
    }
    trace_.emplace_back(buf);
}

long Sentry::Handle(const SyscallRequest& req) {
    long ret;
    switch (req.nr) {
        case kWrite:
            ret = SysWrite(static_cast<int>(req.args[0]), req.args[1],
                           static_cast<size_t>(req.args[2]));
            break;
        case kWritev:
            ret = SysWritev(static_cast<int>(req.args[0]), req.args[1],
                            static_cast<int>(req.args[2]));
            break;
        case kExit:
        case kExitGroup:
            exited_ = true;
            exit_status_ = static_cast<int>(req.args[0]) & 0xff;
            ret = 0;
            break;
        case kBrk:
            ret = SysBrk(req.args[0]);
            break;
        case kUname:
            ret = SysUname(req.args[0]);
            break;
        case kGetpid:
            ret = kGuestPid;
            break;
        case kGetppid:
            ret = kGuestPpid;
            break;
        case kGettid:
            ret = kGuestPid;
            break;
        case kGetuid:
        case kGeteuid:
        case kGetgid:
        case kGetegid:
            // The guest is root inside its own world and nowhere else.
            ret = 0;
            break;
        case kSetTidAddress:
            ret = kGuestPid;
            break;
        default:
            ret = -ENOSYS;
            break;
    }
    Record(req, ret);
    return ret;
}

}  // namespace goblin
