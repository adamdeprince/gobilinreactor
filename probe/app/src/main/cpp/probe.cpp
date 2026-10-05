#include "probe.h"

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/memfd.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <sched.h>
#include <sys/stat.h>
#include <cstdlib>
#include <utility>

// Bionic's UAPI headers lag the kernel on some NDK/platform combinations, and
// this probe deliberately reaches for features newer than its minSdk.
#ifndef SECCOMP_GET_ACTION_AVAIL
#define SECCOMP_GET_ACTION_AVAIL 2
#endif
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif

namespace goblin {
namespace {

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

enum class Status { kOk, kFail, kInfo, kWarn };

const char* StatusTag(Status s) {
    switch (s) {
        case Status::kOk:   return "[ OK ]";
        case Status::kFail: return "[FAIL]";
        case Status::kWarn: return "[WARN]";
        case Status::kInfo: return "[INFO]";
    }
    return "[????]";
}

class Report {
public:
    void Section(const char* title) {
        lines_.push_back("");
        lines_.push_back(std::string("== ") + title + " ");
    }

    __attribute__((format(printf, 4, 5)))
    void Add(Status s, const char* name, const char* fmt, ...) {
        char detail[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(detail, sizeof(detail), fmt, ap);
        va_end(ap);

        char buf[640];
        snprintf(buf, sizeof(buf), "%s %-34s %s", StatusTag(s), name, detail);
        lines_.push_back(buf);
    }

    std::string Text() const {
        std::string out;
        for (const std::string& l : lines_) {
            out += l;
            out += '\n';
        }
        return out;
    }

private:
    std::vector<std::string> lines_;
};

// errno as "ENOSYS (38)" for readability in the report.
std::string Err() {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s (%d)", strerror(errno), errno);
    return buf;
}

std::string ReadFileTrimmed(const char* path, size_t limit = 256) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "<unreadable>";
    // procfs hands back one chunk at a time, so a single read() truncates
    // silently -- which is how /proc/self/status lost everything past VmSwap.
    std::string out;
    char buf[1024];
    ssize_t n;
    while (out.size() < limit && (n = read(fd, buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    if (out.empty()) return "<unreadable>";
    if (out.size() > limit) out.resize(limit);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\0')) out.pop_back();
    return out;
}

// ---------------------------------------------------------------------------
// Executing a probe in a forked child
//
// A blocked execute permission surfaces as SIGSEGV rather than an error return,
// so every attempt to actually jump into a mapping happens in a child process.
// ---------------------------------------------------------------------------

constexpr int kMagic = 42;  // what the test payloads return on success

// Returns >=0 child exit status, or -signo if the child died on a signal.
int RunInChild(int (*fn)(void*), void* arg) {
    pid_t pid = fork();
    if (pid < 0) return -1000;
    if (pid == 0) {
        _exit(fn(arg) & 0xff);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1001;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return -1002;
}

// aarch64: mov w0, #42 ; ret
const uint32_t kRet42[] = {0x52800540u, 0xd65f03c0u};

// aarch64: mov x8, #168 (__NR_getcpu) ; svc #0 ; ret
// Used to prove that a syscall issued *from JIT-mapped memory* -- exactly what
// guest code will do -- is caught by our seccomp filter.
const uint32_t kSvcGetcpu[] = {0xd2801508u, 0xd4000001u, 0xd65f03c0u};

int JumpToPayload(void* arg) {
    void* p = arg;
    __builtin___clear_cache(static_cast<char*>(p), static_cast<char*>(p) + 16);
    int (*fn)() = reinterpret_cast<int (*)()>(p);
    return fn();
}

// Interprets a RunInChild result for a payload that should return kMagic.
void ReportExec(Report* r, Status ok_status, const char* name, int rc,
                const char* extra = "") {
    if (rc == kMagic) {
        r->Add(ok_status, name, "executed%s%s", *extra ? " -- " : "", extra);
    } else if (rc < 0 && rc > -100) {
        r->Add(Status::kFail, name, "died on signal %d (%s)%s%s", -rc,
               strsignal(-rc), *extra ? " -- " : "", extra);
    } else {
        r->Add(Status::kFail, name, "unexpected result %d%s%s", rc,
               *extra ? " -- " : "", extra);
    }
}

}  // namespace
}  // namespace goblin

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

namespace goblin {
namespace {

void ProbeEnvironment(Report* r) {
    r->Section("Environment");

    utsname u{};
    if (uname(&u) == 0) {
        r->Add(Status::kInfo, "kernel", "%s %s %s", u.sysname, u.release, u.machine);
    } else {
        r->Add(Status::kFail, "kernel", "uname: %s", Err().c_str());
    }

    r->Add(Status::kInfo, "selinux context", "%s",
           ReadFileTrimmed("/proc/self/attr/current").c_str());
    r->Add(Status::kInfo, "uid / pid", "%d / %d", getuid(), getpid());

    long ps = sysconf(_SC_PAGESIZE);
    // Android 15+ ships devices with 16 KiB pages. Debian arm64 ELF segments are
    // aligned for 64 KiB, so guest segments can never be mapped file-backed at
    // native alignment anyway -- which is one more reason the loader copies into
    // anonymous memory rather than mapping.
    r->Add(ps == 4096 ? Status::kInfo : Status::kWarn, "page size", "%ld bytes%s", ps,
           ps == 16384 ? "  (16 KiB device)" : "");

    {
        // Android's zygote installs a filter on every app process. Ours stacks on
        // top of it, and the most restrictive result wins.
        const std::string status = ReadFileTrimmed("/proc/self/status", 8192);
        size_t at = status.find("Seccomp:");
        std::string mode = at == std::string::npos
                               ? "field absent"
                               : status.substr(at, status.find('\n', at) - at);
        for (char& c : mode) {
            if (c == '\t') c = ' ';
        }
        r->Add(Status::kInfo, "seccomp (inherited)", "%s", mode.c_str());
    }
    r->Add(Status::kInfo, "vm.max_map_count", "%s",
           ReadFileTrimmed("/proc/sys/vm/max_map_count").c_str());
    r->Add(Status::kInfo, "yama ptrace_scope", "%s",
           ReadFileTrimmed("/proc/sys/kernel/yama/ptrace_scope").c_str());

    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        r->Add(Status::kInfo, "RLIMIT_NOFILE", "%llu soft / %llu hard",
               static_cast<unsigned long long>(rl.rlim_cur),
               static_cast<unsigned long long>(rl.rlim_max));
    }
    if (getrlimit(RLIMIT_AS, &rl) == 0) {
        r->Add(Status::kInfo, "RLIMIT_AS", "%s",
               rl.rlim_cur == RLIM_INFINITY ? "unlimited" : "LIMITED -- check headroom");
    }
}

// ---------------------------------------------------------------------------
// Executable memory
//
// The core question: can we get guest code into executable pages without ever
// asking the kernel to execute a file in our data directory?
// ---------------------------------------------------------------------------

void ProbeAnonExec(Report* r) {
    const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));

    // (1) Straight RWX anonymous mapping. ART's own JIT needs `execmem`, so this
    // is expected to work; it is the simplest path for the loader.
    void* p = mmap(nullptr, ps, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        r->Add(Status::kFail, "anon mmap RWX", "%s", Err().c_str());
    } else {
        memcpy(p, kRet42, sizeof(kRet42));
        ReportExec(r, Status::kOk, "anon mmap RWX", RunInChild(JumpToPayload, p));
        munmap(p, ps);
    }

    // (2) W^X-respecting path: map RW, write, flip to RX. Preferred for the
    // loader -- it never holds a page both writable and executable.
    p = mmap(nullptr, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        r->Add(Status::kFail, "anon RW -> mprotect RX", "mmap: %s", Err().c_str());
    } else {
        memcpy(p, kRet42, sizeof(kRet42));
        if (mprotect(p, ps, PROT_READ | PROT_EXEC) != 0) {
            r->Add(Status::kFail, "anon RW -> mprotect RX", "mprotect: %s", Err().c_str());
        } else {
            ReportExec(r, Status::kOk, "anon RW -> mprotect RX",
                       RunInChild(JumpToPayload, p));
        }
        munmap(p, ps);
    }
}

// memfd-backed execution is the difference between every guest process paying
// full RAM cost for libc and all of them sharing one copy. If this works, the
// loader gets a shared page cache; if it does not, memory use per guest process
// goes up by tens of megabytes and the design needs a different answer.
void ProbeMemfdExec(Report* r) {
    const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));

    auto make_memfd = [&](unsigned flags) -> int {
        int fd = static_cast<int>(syscall(__NR_memfd_create, "goblin-probe", flags));
        if (fd < 0) return -1;
        if (ftruncate(fd, static_cast<off_t>(ps)) != 0) { close(fd); return -1; }
        if (pwrite(fd, kRet42, sizeof(kRet42), 0) != static_cast<ssize_t>(sizeof(kRet42))) {
            close(fd);
            return -1;
        }
        return fd;
    };

    int fd = make_memfd(MFD_CLOEXEC);
    if (fd < 0) {
        r->Add(Status::kFail, "memfd_create", "%s", Err().c_str());
        return;
    }
    r->Add(Status::kOk, "memfd_create", "available");

    for (int shared = 0; shared <= 1; ++shared) {
        const char* name = shared ? "memfd mmap R+X MAP_SHARED"
                                  : "memfd mmap R+X MAP_PRIVATE";
        void* p = mmap(nullptr, ps, PROT_READ | PROT_EXEC,
                       shared ? MAP_SHARED : MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            r->Add(Status::kFail, name, "%s  (no shared page cache this way)",
                   Err().c_str());
            continue;
        }
        ReportExec(r, Status::kOk, name, RunInChild(JumpToPayload, p));
        munmap(p, ps);
    }

    // Sealing the fd read-only before mapping it executable is the strongest
    // form of the argument that this is not self-modifying code.
    close(fd);
    fd = make_memfd(MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd >= 0) {
        if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
            r->Add(Status::kWarn, "memfd F_SEAL_WRITE", "%s", Err().c_str());
        } else {
            void* p = mmap(nullptr, ps, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) {
                r->Add(Status::kFail, "sealed memfd exec-map", "%s", Err().c_str());
            } else {
                ReportExec(r, Status::kOk, "sealed memfd exec-map",
                           RunInChild(JumpToPayload, p));
                munmap(p, ps);
            }
        }
        close(fd);
    }
}

}  // namespace
}  // namespace goblin

// ---------------------------------------------------------------------------
// The restrictions we are routing around
//
// These are expected to FAIL. Their failure is what forces the design: it is
// why Termux is pinned to targetSdkVersion 28 and why our loader must never
// hand a guest file to the kernel. Confirming them per-device matters, because
// if one unexpectedly succeeds the loader has a cheaper option available.
// ---------------------------------------------------------------------------

namespace goblin {
namespace {

bool WriteFileMode(const std::string& path, const void* data, size_t len, mode_t mode) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    bool ok = write(fd, data, len) == static_cast<ssize_t>(len);
    close(fd);
    if (ok) chmod(path.c_str(), mode);
    return ok;
}

bool CopyFile(const std::string& from, const std::string& to, mode_t mode) {
    int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (out < 0) { close(in); return false; }
    char buf[65536];
    ssize_t n;
    bool ok = true;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, static_cast<size_t>(n)) != n) { ok = false; break; }
    }
    if (n < 0) ok = false;
    close(in);
    close(out);
    if (ok) chmod(to.c_str(), mode);
    return ok;
}

struct ExecveArg {
    const char* path;
};

int DoExecve(void* a) {
    auto* e = static_cast<ExecveArg*>(a);
    char* const argv[] = {const_cast<char*>(e->path), nullptr};
    char* const envp[] = {nullptr};
    execve(e->path, argv, envp);
    // Reaching here means execve failed; smuggle errno out through the exit code.
    return 100 + (errno & 0x3f);
}

void ProbeFileExecRestrictions(Report* r, const ProbePaths& paths) {
    r->Section("Restrictions we route around (failures here are expected)");

    const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const std::string payload = paths.internal_data + "/payload.bin";

    // File-backed PROT_EXEC out of the app data directory. This is the mapping
    // the guest's own dynamic linker would try to make, and the reason the
    // sentry has to service guest exec-mappings by copying instead.
    if (!WriteFileMode(payload, kRet42, sizeof(kRet42), 0600)) {
        r->Add(Status::kWarn, "app-data file mmap PROT_EXEC", "setup failed: %s",
               Err().c_str());
    } else {
        int fd = open(payload.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            r->Add(Status::kWarn, "app-data file mmap PROT_EXEC", "open: %s", Err().c_str());
        } else {
            void* p = mmap(nullptr, ps, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) {
                r->Add(Status::kInfo, "app-data file mmap PROT_EXEC",
                       "blocked as expected: %s", Err().c_str());
            } else {
                int rc = RunInChild(JumpToPayload, p);
                r->Add(rc == kMagic ? Status::kWarn : Status::kInfo,
                       "app-data file mmap PROT_EXEC",
                       rc == kMagic ? "PERMITTED -- loader could map instead of copy"
                                    : "mapped but not executable");
                munmap(p, ps);
            }
            close(fd);
        }
    }

    // execve() of a data-directory file: the API 29 W^X restriction itself.
    const std::string copied = paths.internal_data + "/true";
    if (CopyFile("/system/bin/true", copied, 0700)) {
        ExecveArg a{copied.c_str()};
        int rc = RunInChild(DoExecve, &a);
        if (rc >= 100) {
            r->Add(Status::kInfo, "execve app-data file", "blocked as expected: %s",
                   strerror(rc - 100));
        } else {
            r->Add(Status::kWarn, "execve app-data file", "PERMITTED (rc=%d)", rc);
        }
    } else {
        r->Add(Status::kWarn, "execve app-data file", "setup failed: %s", Err().c_str());
    }

    // Control: a system binary must still be executable, or the probe is broken.
    {
        ExecveArg a{"/system/bin/true"};
        int rc = RunInChild(DoExecve, &a);
        r->Add(rc == 0 ? Status::kOk : Status::kFail, "execve /system/bin/true (control)",
               rc == 0 ? "ok" : "rc=%d -- probe methodology suspect", rc);
    }

    // dlopen from the data directory vs. from the APK's native library dir.
    // The latter is the one escape hatch Android does allow, and it is where
    // our own code (sentry, kitty, CPython) legitimately lives.
    const std::string self_lib = paths.native_lib_dir + "/libgoblinprobe.so";
    const std::string copied_lib = paths.internal_data + "/libcopied.so";
    if (CopyFile(self_lib, copied_lib, 0700)) {
        void* h = dlopen(copied_lib.c_str(), RTLD_NOW);
        if (h == nullptr) {
            const char* why = dlerror();
            r->Add(Status::kInfo, "dlopen from app data", "blocked as expected: %s",
                   why ? why : "(no dlerror)");
        } else {
            r->Add(Status::kWarn, "dlopen from app data", "PERMITTED");
            dlclose(h);
        }
    } else {
        r->Add(Status::kWarn, "dlopen from app data", "setup failed (no %s)",
               self_lib.c_str());
    }

    {
        void* h = dlopen(self_lib.c_str(), RTLD_NOW);
        r->Add(h ? Status::kOk : Status::kFail, "dlopen from nativeLibraryDir",
               h ? "ok -- our own code has an executable home" : "%s", dlerror());
        if (h) dlclose(h);
    }

    // No user namespaces on Android, so none of the usual container primitives
    // are available. The sentry has to provide isolation itself.
    if (syscall(__NR_unshare, CLONE_NEWUSER) == 0) {
        r->Add(Status::kWarn, "unshare(CLONE_NEWUSER)", "PERMITTED -- unexpected");
    } else {
        r->Add(Status::kInfo, "unshare(CLONE_NEWUSER)", "blocked as expected: %s",
               Err().c_str());
    }
}

}  // namespace
}  // namespace goblin

// ---------------------------------------------------------------------------
// Seccomp trapping -- the load-bearing probe
//
// The entire sentry design depends on SECCOMP_RET_TRAP behaving the way gVisor's
// systrap platform needs: every guest syscall raises SIGSYS, the handler can read
// the syscall number and arguments out of the signal context, inject a return
// value, and resume the guest. If this does not work, or is too slow, there is no
// project.
//
// Note that signal dispositions are process-wide even though seccomp filters are
// per-thread, so installing this handler briefly overrides bionic's own SIGSYS
// handler for the whole process. The probe restores it afterwards.
// ---------------------------------------------------------------------------

namespace goblin {
namespace {

constexpr long kInjectedReturn = 0x5a5a;

volatile sig_atomic_t g_sigsys_hits = 0;
volatile unsigned long g_seen_nr = 0;
volatile unsigned long g_seen_arg0 = 0;
volatile unsigned long g_seen_arg1 = 0;

void SigsysHandler(int /*sig*/, siginfo_t* si, void* vctx) {
    auto* uc = static_cast<ucontext_t*>(vctx);
    g_sigsys_hits++;
    g_seen_nr = static_cast<unsigned long>(si->si_syscall);
    g_seen_arg0 = uc->uc_mcontext.regs[0];
    g_seen_arg1 = uc->uc_mcontext.regs[1];
    // On aarch64 the saved PC already points past the SVC and the syscall has been
    // aborted, so writing x0 is exactly "return this value from the syscall".
    uc->uc_mcontext.regs[0] = static_cast<unsigned long>(kInjectedReturn);
}

// Traps __NR_getcpu and allows everything else. getcpu is chosen because bionic
// never calls it behind our back, so every trap we count is one we caused.
bool InstallTrapFilter(int* out_errno) {
    sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getcpu, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog prog{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])),
                    filter};

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        *out_errno = errno;
        return false;
    }
    // No TSYNC: the filter must apply to this thread only, so that sentry threads
    // in the same process stay unfiltered.
    if (syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) != 0) {
        *out_errno = errno;
        return false;
    }
    return true;
}

struct SeccompFindings {
    bool installed = false;
    int install_errno = 0;
    bool trapped = false;
    bool nr_correct = false;
    bool args_correct = false;
    bool return_injected = false;
    bool jit_svc_trapped = false;
    double ns_per_trap = 0.0;
};

double NowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1e9 + static_cast<double>(ts.tv_nsec);
}

void* SeccompThread(void* arg) {
    auto* f = static_cast<SeccompFindings*>(arg);

    if (!InstallTrapFilter(&f->install_errno)) return nullptr;
    f->installed = true;

    // A plain trapped syscall with recognisable arguments.
    constexpr unsigned long kA0 = 0xdead0001, kA1 = 0xdead0002;
    g_sigsys_hits = 0;
    long ret = syscall(__NR_getcpu, kA0, kA1, 0);
    f->trapped = g_sigsys_hits > 0;
    f->nr_correct = g_seen_nr == __NR_getcpu;
    f->args_correct = g_seen_arg0 == kA0 && g_seen_arg1 == kA1;
    f->return_injected = ret == kInjectedReturn;

    // The real scenario: an SVC executed out of a JIT-mapped anonymous page,
    // which is how every guest syscall will actually arrive.
    const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* p = mmap(nullptr, ps, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        memcpy(p, kSvcGetcpu, sizeof(kSvcGetcpu));
        __builtin___clear_cache(static_cast<char*>(p), static_cast<char*>(p) + 16);
        g_sigsys_hits = 0;
        long (*fn)() = reinterpret_cast<long (*)()>(p);
        long jit_ret = fn();
        f->jit_svc_trapped = g_sigsys_hits > 0 && jit_ret == kInjectedReturn;
        munmap(p, ps);
    }

    // Round-trip cost. This is the number that decides whether a syscall-heavy
    // guest workload is tolerable; gVisor's systrap lands in the low microseconds.
    constexpr int kIters = 50000;
    double t0 = NowNs();
    for (int i = 0; i < kIters; ++i) syscall(__NR_getcpu, 0, 0, 0);
    f->ns_per_trap = (NowNs() - t0) / kIters;

    return nullptr;
}

void ProbeSeccomp(Report* r) {
    r->Section("Seccomp trapping (the sentry depends on all of this)");

    for (auto [action, name] : {std::pair<unsigned, const char*>{SECCOMP_RET_TRAP, "SECCOMP_RET_TRAP"},
                                std::pair<unsigned, const char*>{SECCOMP_RET_USER_NOTIF, "SECCOMP_RET_USER_NOTIF"}}) {
        unsigned a = action;
        bool avail = syscall(__NR_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &a) == 0;
        r->Add(avail ? Status::kOk : Status::kWarn, name,
               avail ? "available" : "unavailable: %s", Err().c_str());
    }

    // Take over SIGSYS on an alternate stack, so a guest that has exhausted its
    // own stack can still be serviced.
    stack_t ss{};
    ss.ss_size = SIGSTKSZ * 4;
    ss.ss_sp = malloc(ss.ss_size);
    ss.ss_flags = 0;
    bool alt_ok = ss.ss_sp != nullptr && sigaltstack(&ss, nullptr) == 0;
    r->Add(alt_ok ? Status::kOk : Status::kWarn, "sigaltstack for SIGSYS",
           alt_ok ? "installed" : "%s", Err().c_str());

    struct sigaction sa {}, old {};
    sa.sa_sigaction = SigsysHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSYS, &sa, &old) != 0) {
        r->Add(Status::kFail, "SIGSYS handler", "%s", Err().c_str());
        return;
    }

    SeccompFindings f;
    pthread_t th;
    if (pthread_create(&th, nullptr, SeccompThread, &f) != 0) {
        r->Add(Status::kFail, "seccomp probe thread", "%s", Err().c_str());
        sigaction(SIGSYS, &old, nullptr);
        return;
    }
    pthread_join(th, nullptr);

    if (!f.installed) {
        r->Add(Status::kFail, "install per-thread filter", "%s",
               strerror(f.install_errno));
        sigaction(SIGSYS, &old, nullptr);
        return;
    }
    r->Add(Status::kOk, "install per-thread filter", "no TSYNC, thread-scoped");
    r->Add(f.trapped ? Status::kOk : Status::kFail, "SIGSYS delivered on trap",
           f.trapped ? "yes" : "NO -- design is not viable");
    r->Add(f.nr_correct ? Status::kOk : Status::kFail, "si_syscall carries nr",
           f.nr_correct ? "correct" : "got %lu, expected %d", g_seen_nr, __NR_getcpu);
    r->Add(f.args_correct ? Status::kOk : Status::kFail, "args readable from ucontext",
           f.args_correct ? "x0/x1 match" : "x0=%#lx x1=%#lx", g_seen_arg0, g_seen_arg1);
    r->Add(f.return_injected ? Status::kOk : Status::kFail, "return value injectable",
           f.return_injected ? "writing x0 works" : "NO -- cannot emulate syscalls");
    r->Add(f.jit_svc_trapped ? Status::kOk : Status::kFail, "SVC from JIT page trapped",
           f.jit_svc_trapped ? "yes -- guest path proven" : "NO -- guest path broken");
    r->Add(f.ns_per_trap > 0 && f.ns_per_trap < 20000 ? Status::kOk : Status::kWarn,
           "trap round-trip", "%.0f ns/syscall  (~%.2f us)", f.ns_per_trap,
           f.ns_per_trap / 1000.0);

    // The filter must not have leaked to this thread, or the sentry could not
    // make syscalls of its own.
    g_sigsys_hits = 0;
    syscall(__NR_getcpu, 0, 0, 0);
    bool leaked = g_sigsys_hits > 0;
    r->Add(leaked ? Status::kFail : Status::kOk, "filter stayed thread-local",
           leaked ? "LEAKED to other threads" : "other threads unfiltered");

    sigaction(SIGSYS, &old, nullptr);
}

}  // namespace
}  // namespace goblin

// ---------------------------------------------------------------------------
// Processes and address spaces
//
// Guest processes need real, separate address spaces -- fork(2) has no meaning
// otherwise. The plan is one Android process per guest address space, so the
// sentry has to be able to fork off a native thread, inspect and write another
// process's memory, and reserve large sparse regions for guest layouts.
// ---------------------------------------------------------------------------

namespace goblin {
namespace {

struct ForkFindings {
    bool forked = false;
    bool child_ran = false;
    int err = 0;
};

void* ForkThread(void* arg) {
    auto* f = static_cast<ForkFindings*>(arg);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        f->err = errno;
        return nullptr;
    }
    pid_t pid = fork();
    if (pid < 0) {
        f->err = errno;
        close(sv[0]);
        close(sv[1]);
        return nullptr;
    }
    if (pid == 0) {
        // Deliberately touches nothing but raw syscalls: a forked child of an
        // app process has an unusable ART runtime, and guest stubs must live
        // within that constraint too.
        char c = 'g';
        write(sv[1], &c, 1);
        _exit(0);
    }
    f->forked = true;
    char c = 0;
    f->child_ran = read(sv[0], &c, 1) == 1 && c == 'g';
    int st = 0;
    waitpid(pid, &st, 0);
    close(sv[0]);
    close(sv[1]);
    return nullptr;
}

void ProbeProcesses(Report* r) {
    r->Section("Processes and address spaces");

    ForkFindings ff;
    pthread_t th;
    if (pthread_create(&th, nullptr, ForkThread, &ff) == 0) {
        pthread_join(th, nullptr);
        if (ff.forked && ff.child_ran) {
            r->Add(Status::kOk, "fork() off a native thread", "child ran and reported back");
        } else {
            r->Add(Status::kFail, "fork() off a native thread", "%s",
                   ff.err ? strerror(ff.err) : "child did not report");
        }
    } else {
        r->Add(Status::kFail, "fork() off a native thread", "pthread_create: %s",
               Err().c_str());
    }

    // process_vm_readv / writev: how the sentry moves data in and out of a guest
    // address space without a round trip through the guest itself.
    {
        int pipefd[2];
        if (pipe(pipefd) == 0) {
            volatile unsigned long marker = 0xfeedfacecafebeefUL;
            pid_t pid = fork();
            if (pid == 0) {
                unsigned long addr = reinterpret_cast<unsigned long>(&marker);
                write(pipefd[1], &addr, sizeof(addr));
                pause();
                _exit(0);
            } else if (pid > 0) {
                unsigned long addr = 0;
                bool got = read(pipefd[0], &addr, sizeof(addr)) == sizeof(addr);
                unsigned long readback = 0;
                iovec local{&readback, sizeof(readback)};
                iovec remote{reinterpret_cast<void*>(addr), sizeof(readback)};
                ssize_t n = got ? process_vm_readv(pid, &local, 1, &remote, 1, 0) : -1;
                if (n == static_cast<ssize_t>(sizeof(readback)) &&
                    readback == 0xfeedfacecafebeefUL) {
                    r->Add(Status::kOk, "process_vm_readv", "cross-process read works");
                } else {
                    r->Add(Status::kFail, "process_vm_readv", "%s",
                           n < 0 ? Err().c_str() : "value mismatch");
                }
                kill(pid, SIGKILL);
                int st = 0;
                waitpid(pid, &st, 0);
            }
            close(pipefd[0]);
            close(pipefd[1]);
        }
    }

    // ptrace is the fallback platform if systrap turns out to be unusable, and
    // is also how the sentry would single-step a misbehaving guest.
    {
        pid_t pid = fork();
        if (pid == 0) {
            pause();
            _exit(0);
        } else if (pid > 0) {
            bool attached = ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) == 0;
            if (attached) {
                int st = 0;
                waitpid(pid, &st, 0);
                ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
            }
            r->Add(attached ? Status::kOk : Status::kWarn, "ptrace own child",
                   attached ? "attach ok" : "%s", Err().c_str());
            kill(pid, SIGKILL);
            int st = 0;
            waitpid(pid, &st, 0);
        }
    }

    // userfaultfd would let the sentry implement demand paging and copy-on-write
    // fork cheaply. Android usually restricts it; if it is gone, fork() has to
    // copy eagerly.
    {
        long fd = syscall(__NR_userfaultfd, O_CLOEXEC);
        if (fd >= 0) {
            r->Add(Status::kOk, "userfaultfd", "available -- lazy CoW fork possible");
            close(static_cast<int>(fd));
        } else {
            r->Add(Status::kWarn, "userfaultfd", "%s -- fork must copy eagerly",
                   Err().c_str());
        }
    }

    {
        long res = syscall(__NR_membarrier, 0 /* MEMBARRIER_CMD_QUERY */, 0, 0);
        if (res >= 0) {
            r->Add(Status::kOk, "membarrier", "supported cmds mask %#lx",
                   static_cast<unsigned long>(res));
        } else {
            r->Add(Status::kWarn, "membarrier", "%s", Err().c_str());
        }
    }
}

void ProbeAddressSpace(Report* r) {
    // How much contiguous address space can be reserved for a guest layout.
    const size_t gib = 1UL << 30;
    size_t largest = 0;
    for (size_t n : {size_t(1), size_t(4), size_t(16), size_t(64), size_t(256)}) {
        void* p = mmap(nullptr, n * gib, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) break;
        munmap(p, n * gib);
        largest = n;
    }
    r->Add(largest >= 16 ? Status::kOk : Status::kWarn, "sparse VA reservation",
           "largest PROT_NONE reservation: %zu GiB", largest);

    const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* base = mmap(nullptr, ps, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base != MAP_FAILED) {
        void* again = mmap(base, ps, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        bool honoured = again == MAP_FAILED && errno == EEXIST;
        r->Add(honoured ? Status::kOk : Status::kWarn, "MAP_FIXED_NOREPLACE",
               honoured ? "honoured -- safe guest layout placement"
                        : "not honoured -- placement needs a bookkeeping map");
        if (again != MAP_FAILED && again != base) munmap(again, ps);
        munmap(base, ps);
    }
}

}  // namespace

std::string RunAllProbes(const ProbePaths& paths) {
    Report r;
    r.Add(Status::kInfo, "GoblinReactor probe", "phase 0 device capability report");

    ProbeEnvironment(&r);

    r.Section("Executable memory (how guest code gets mapped)");
    ProbeAnonExec(&r);
    ProbeMemfdExec(&r);

    ProbeFileExecRestrictions(&r, paths);
    ProbeSeccomp(&r);
    ProbeProcesses(&r);
    ProbeAddressSpace(&r);

    r.Section("Full /proc/self/status");
    r.Add(Status::kInfo, "status", "\n%s",
          ReadFileTrimmed("/proc/self/status", 8192).c_str());

    return r.Text();
}

}  // namespace goblin
