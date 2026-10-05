#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/memfd.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

static void require(bool condition, const char* name) {
    if (!condition) { printf("FAIL %s errno=%d (%s)\n", name, errno, strerror(errno)); exit(1); }
    printf("PASS %s\n", name);
}
static void show(const char* path) {
    char bytes[2048]; int fd = open(path, O_RDONLY); ssize_t n = fd < 0 ? -1 : read(fd, bytes, sizeof(bytes)-1);
    if (n >= 0) { bytes[n] = 0; printf("%s: %s\n", path, bytes); }
    else printf("%s: errno=%d\n", path, errno);
    if (fd >= 0) close(fd);
}
static void trap(int, siginfo_t*, void* raw) { static_cast<ucontext_t*>(raw)->uc_mcontext.regs[0] = 4242; }
static int clone_worker(void* value) { *static_cast<volatile int*>(value) = 42; return 0; }
static double now() { timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0); prctl(PR_SET_NAME, "gz-helper");
    if (argc != 4) return 2;
    int count = atoi(argv[1]), seconds = atoi(argv[2]);
    if (count < 1 || seconds < 1) return 2;
    printf("pid=%d ppid=%d uid=%d pagesize=%ld cpus=%ld children=%d seconds=%d\n",
        getpid(), getppid(), getuid(), sysconf(_SC_PAGESIZE), sysconf(_SC_NPROCESSORS_ONLN), count, seconds);
    show("/proc/self/attr/current"); show("/proc/self/cgroup");
    int direct = open(argv[3], O_RDWR);
    printf("private file path open: fd=%d errno=%d (denial expected in isolated service)\n", direct, direct < 0 ? errno : 0);
    if (direct >= 0) close(direct);
    direct = open("/proc/self/fd/3", O_RDWR);
    printf("private file /proc/self/fd reopen: fd=%d errno=%d\n", direct, direct < 0 ? errno : 0);
    if (direct >= 0) close(direct);
    require(pwrite(3, "isolated-write", 14, 4096) == 14 && fsync(3) == 0, "passed private disk FD write/fsync");
    void* disk = mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, 3, 0);
    require(disk != MAP_FAILED, "passed private disk FD shared mapping");
    munmap(disk, 65536);
    require(write(4, "network-ok\n", 11) == 11, "passed TCP socket write");
    direct = socket(AF_INET, SOCK_STREAM, 0);
    printf("direct TCP socket: fd=%d errno=%d (denial expected in isolated service)\n", direct, direct < 0 ? errno : 0);
    if (direct >= 0) close(direct);

    int ram = syscall(SYS_memfd_create, "zygote-uml-ram", MFD_CLOEXEC | MFD_ALLOW_SEALING | 0x10);
    if (ram < 0 && errno == EINVAL) ram = syscall(SYS_memfd_create, "zygote-uml-ram", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    require(ram >= 0 && ftruncate(ram, 65536) == 0, "own executable memfd");
    auto* code = static_cast<uint32_t*>(mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, ram, 0));
    require(code != MAP_FAILED, "memfd shared writable alias");
    code[0] = 0x52800540; code[1] = 0xd65f03c0; // mov w0,#42; ret
    __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code+2));
    void* reserved = mmap(nullptr, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(reserved != MAP_FAILED, "reserve guest address range");
    void* executable = mmap(reserved, 65536, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED, ram, 0);
    require(executable == reserved && reinterpret_cast<int(*)()>(executable)() == 42, "memfd fixed executable alias runs ARM64 code");
    munmap(executable, 65536); munmap(code, 65536); close(ram);

    volatile int shared = 0;
    void* stack = mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(stack != MAP_FAILED, "clone stack");
    pid_t sibling = clone(clone_worker, static_cast<char*>(stack) + 65536, CLONE_VM | SIGCHLD, const_cast<int*>(&shared));
    int status = 0;
    require(sibling > 0 && waitpid(sibling, &status, 0) == sibling && status == 0 && shared == 42, "CLONE_VM process shares memory");
    munmap(stack, 65536);

    pid_t seccomp = fork(); require(seccomp >= 0, "fork syscall-trap check");
    if (!seccomp) {
        struct sigaction action{}; action.sa_sigaction = trap; action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGSYS, &action, nullptr) < 0) _exit(10);
        sock_filter instructions[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
        sock_fprog program{static_cast<unsigned short>(sizeof(instructions)/sizeof(instructions[0])), instructions};
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 || syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) < 0) _exit(11);
        _exit(syscall(SYS_getppid) == 4242 ? 0 : 12);
    }
    require(waitpid(seccomp, &status, 0) == seccomp && status == 0, "stacked seccomp SIGSYS trapping");

    size_t size = (static_cast<size_t>(count) + 1) * sizeof(uint64_t);
    auto* counters = static_cast<uint64_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    require(counters != MAP_FAILED, "shared worker heartbeat memory");
    int busy = sysconf(_SC_NPROCESSORS_ONLN);
    double deadline = now() + seconds;
    pid_t parent = getpid(); std::vector<pid_t> workers;
    for (int i = 0; i < count; ++i) {
        pid_t child = fork(); require(child >= 0, "fork workload child");
        if (!child) {
            prctl(PR_SET_NAME, "gz-worker"); prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() != parent) _exit(20);
            uint64_t value = i + 1;
            while (!__atomic_load_n(counters, __ATOMIC_ACQUIRE) && now() < deadline + 10) {
                if (i < busy) for (int n = 0; n < 200000; ++n) value = value * 6364136223846793005ULL + 1;
                else { timespec delay{0, 20000000}; nanosleep(&delay, nullptr); }
                asm volatile("" : "+r"(value));
                __atomic_add_fetch(counters + i + 1, 1, __ATOMIC_RELAXED);
            }
            _exit(0);
        }
        workers.push_back(child);
    }
    printf("READY children=%d busy=%d helper=%d deadline_seconds=%d\n", count, busy, parent, seconds);
    bool failed = false;
    for (int elapsed = 0; now() < deadline; ++elapsed) {
        timespec delay{1, 0}; nanosleep(&delay, nullptr);
        for (pid_t child : workers) if (waitpid(child, &status, WNOHANG) != 0) {
            printf("FAIL child disappeared pid=%d status=%d errno=%d\n", child, status, errno); failed = true; break;
        }
        if (failed) break;
        if (elapsed % 15 == 0) printf("ALIVE children=%d elapsed=%d\n", count, elapsed + 1);
    }
    __atomic_store_n(counters, 1, __ATOMIC_RELEASE);
    for (pid_t child : workers) {
        int returned; do { returned = waitpid(child, &status, 0); } while (returned < 0 && errno == EINTR);
        if (returned != child || status != 0) failed = true;
    }
    for (int i = 0; i < count; ++i) if (!__atomic_load_n(counters + i + 1, __ATOMIC_RELAXED)) failed = true;
    printf("%s workload children=%d seconds=%d\n", failed ? "FAIL" : "PASS", count, seconds);
    return failed ? 1 : 0;
}
