#include "stub.h"
#include "stub_process.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <elf.h>
#include <memory>
#include <vector>
#include <linux/auxvec.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/auxv.h>
#include <time.h>
#include <unistd.h>
#include "channel.h"
#include "guest_layout.h"
#include "guest_memory.h"
#include "stub_abi.h"
#include "stub_filter.h"
#include "stub_offsets.h"

extern "C" const unsigned char goblin_stub_blob_start[], goblin_stub_blob_end[];

namespace goblin {
namespace {
struct Runtime {
    void* region = MAP_FAILED;
    ~Runtime() { if (region != MAP_FAILED) munmap(region, STUB_REGION_SIZE); }
};

bool MapStack(std::string* err) {
    const size_t ps = sysconf(_SC_PAGESIZE);
    void* trampoline = mmap(reinterpret_cast<void*>(GuestWindow::signal_trampoline()), ps,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (trampoline == MAP_FAILED) { *err = "mapping signal trampoline"; return false; }
    const uint32_t code[] = {0xd2801168, 0xd4000001, 0xd4200000}; // mov x8,139; svc; brk
    memcpy(trampoline, code, sizeof(code));
    __builtin___clear_cache(static_cast<char*>(trampoline), static_cast<char*>(trampoline) + sizeof(code));
    if (mprotect(trampoline, ps, PROT_READ | PROT_EXEC) != 0) { *err = "protecting signal trampoline"; return false; }
    void* p = mmap(reinterpret_cast<void*>(GuestWindow::stack_bottom()),
                   GuestWindow::kStackSize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p != MAP_FAILED) return true;
    *err = std::string("mapping guest stack: ") + strerror(errno);
    return false;
}

uintptr_t InitialStack(const LoadedImage& img, const Sentry& sentry, uintptr_t test_gate,
                       uintptr_t channel_state, std::string* err) {
    GuestMemory here;
    uintptr_t p = GuestWindow::stack_top();
    unsigned char random[16];
    size_t filled = 0;
    while (filled < sizeof(random)) {
        long n = syscall(__NR_getrandom, random + filled, sizeof(random) - filled, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { *err = "could not obtain initial stack randomness"; return 0; }
        filled += static_cast<size_t>(n);
    }
    p -= sizeof(random);
    const uintptr_t at_random = p;
    here.Write(p, random, sizeof(random));
    auto strings = [&](const std::vector<std::string>& values, std::vector<uint64_t>* addresses) {
        for (const auto& value : values) {
            if (value.size() > 131072 || p - GuestWindow::stack_bottom() < value.size() + 4096) return false;
            p -= value.size() + 1;
            if (!here.Write(p, value.c_str(), value.size() + 1)) return false;
            addresses->push_back(p);
        }
        return true;
    };
    std::vector<uint64_t> argv, envp, execfn;
    if (!strings(sentry.arguments, &argv) || !strings(sentry.environment, &envp) || argv.empty() ||
        !strings({sentry.files().executable}, &execfn)) {
        *err = "arguments exceed initial stack limits"; return 0;
    }
    p &= ~uintptr_t(15);
    std::vector<uint64_t> words = {argv.size()};
    words.insert(words.end(), argv.begin(), argv.end()); words.push_back(0);
    words.insert(words.end(), envp.begin(), envp.end()); words.push_back(0);
    words.insert(words.end(), {
        AT_PAGESZ, static_cast<uint64_t>(sysconf(_SC_PAGESIZE)),
        AT_ENTRY, img.entry, AT_PHDR, img.phdr, AT_PHENT, sizeof(Elf64_Phdr),
        AT_PHNUM, img.phnum, AT_BASE, img.interpreter_base, AT_RANDOM, at_random,
        AT_UID, sentry.credentials.uid, AT_EUID, sentry.credentials.euid,
        AT_GID, sentry.credentials.gid, AT_EGID, sentry.credentials.egid, AT_SECURE, (sentry.credentials.uid != sentry.credentials.euid || sentry.credentials.gid != sentry.credentials.egid) ? 1u : 0u,
        AT_CLKTCK, 100, AT_EXECFN, execfn.front(),
        AT_HWCAP, getauxval(AT_HWCAP) & ~(1UL << 22), AT_HWCAP2, 0,
    });
    if (test_gate != 0) {
        // Test fixtures deliberately know the gate address; secrecy is never
        // part of the security argument. Ordinary guests get no test entries.
        words.insert(words.end(), {0x6000, test_gate, 0x6001,
            reinterpret_cast<uintptr_t>(&syscall), 0x6002, channel_state});
    }
    words.insert(words.end(), {AT_NULL, 0});
    const size_t bytes = words.size() * sizeof(uint64_t);
    const uintptr_t sp = (p - bytes) & ~uintptr_t(15);
    here.Write(sp, words.data(), bytes);
    return sp;
}

bool PrepareRuntime(const LoadedImage& img, const Sentry& sentry, SyscallChannel& channel,
                    bool expose_test_gate, Runtime* runtime, const StubContext* resume, std::string* err) {
    runtime->region = mmap(nullptr, STUB_REGION_SIZE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (runtime->region == MAP_FAILED) { *err = strerror(errno); return false; }
    const uintptr_t base = reinterpret_cast<uintptr_t>(runtime->region);
    const size_t size = goblin_stub_blob_end - goblin_stub_blob_start;
    if (size > STUB_CODE_SIZE) { *err = "stub image exceeds its code region"; return false; }
    memcpy(runtime->region, goblin_stub_blob_start, size);
    __builtin___clear_cache(static_cast<char*>(runtime->region),
                            static_cast<char*>(runtime->region) + size);
    auto* boot = reinterpret_cast<StubBoot*>(base + STUB_BOOT_OFFSET);
    boot->stack_top = base + STUB_REGION_SIZE;
    boot->region_start = base;
    boot->region_end = base + STUB_REGION_SIZE;
    boot->guest_start = GuestWindow::start();
    boot->guest_end = GuestWindow::end();
    boot->guest_low_start = GuestWindow::kLowStart;
    boot->guest_low_end = GuestWindow::kLowEnd;
    boot->guest_low_shadow = GuestWindow::low_shadow();
    if (base < GuestWindow::kLowEnd || channel.address() < GuestWindow::kLowEnd ||
        GuestWindow::start() < GuestWindow::kLowEnd) {
        *err = "stub capabilities overlap the guest low range"; return false;
    }
    boot->guest_entry = img.start_entry ? img.start_entry : img.entry;
    boot->guest_sp = resume ? resume->machine.sp : InitialStack(img, sentry, expose_test_gate ? base + STUB_GATE_OFFSET : 0,
                                  channel.address(), err);
    if (boot->guest_sp == 0) return false;
    if (resume) { boot->resume = 1; boot->context = *resume; }
    boot->shared_start = channel.address();
    boot->shared_size = channel.mapping_size();
    boot->signal_stack = base + STUB_SIGNAL_STACK_OFFSET;
    boot->signal_stack_size = STUB_SIGNAL_STACK_SIZE;
    boot->parent_pid = getpid();
    boot->shared_memory_fd = sentry.shared_memory ? sentry.shared_memory->file->fd : -1;
    boot->transfer_fd = channel.transfer_fd();
    if (boot->transfer_fd <= 0 || boot->shared_memory_fd == 0) { *err = "reserved capability descriptor is unavailable"; return false; }
    rlimit files{};
    if (getrlimit(RLIMIT_NOFILE, &files) != 0 || files.rlim_max > 1048576) {
        *err = "unsupported descriptor limit for stub cleanup";
        return false;
    }
    boot->fd_limit = files.rlim_max;
    auto filter = BuildStubFilter(base + STUB_SYSCALL_RETURN_OFFSET,
                                   GuestWindow::start(), GuestWindow::kSize,
                                   channel.address() + offsetof(StubShared, state), boot->shared_memory_fd, boot->transfer_fd);
    if (filter.size() > STUB_MAX_FILTER) { *err = "stub filter is too large"; return false; }
    boot->filter_size = filter.size();
    std::copy(filter.begin(), filter.end(), boot->filter);
    if (mprotect(runtime->region, STUB_CODE_SIZE, PROT_READ | PROT_EXEC) != 0 ||
        mprotect(boot, STUB_CODE_SIZE, PROT_READ) != 0) {
        *err = std::string("protecting stub runtime: ") + strerror(errno);
        return false;
    }
    return true;
}

bool AuditStub(pid_t child, uintptr_t runtime, const SyscallChannel& channel, int shared_fd, int transfer_fd,
               std::string* err) {
    const std::string prefix = "/proc/" + std::to_string(child);
    FILE* maps = fopen((prefix + "/maps").c_str(), "re");
    if (!maps) { *err = "could not audit child mappings"; return false; }
    char line[1024];
    bool ok = true;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long start = 0, end = 0;
        if (sscanf(line, "%lx-%lx", &start, &end) != 2) { ok = false; break; }
        const auto inside = [&](uintptr_t lo, uintptr_t hi) {
            return start >= lo && end <= hi && end > start;
        };
        if (!inside(GuestWindow::start(), GuestWindow::end()) &&
            !inside(GuestWindow::kLowStart, GuestWindow::kLowEnd) &&
            !inside(runtime, runtime + STUB_REGION_SIZE) &&
            !inside(channel.address(), channel.address() + channel.mapping_size())) {
            ok = false;
            break;
        }
    }
    if (ferror(maps)) ok = false;
    fclose(maps);
    if (!ok) { *err = "inherited mapping survived stub startup"; return false; }
    DIR* dir = opendir((prefix + "/fd").c_str());
    if (!dir) { *err = "could not audit child descriptors"; return false; }
    errno = 0;
    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] != '.' && entry->d_name != std::to_string(shared_fd) &&
            entry->d_name != std::to_string(transfer_fd)) { ok = false; break; }
    }
    if (errno) ok = false;
    closedir(dir);
    if (!ok) *err = "host descriptor survived stub startup";
    return ok;
}
}

StubProcess::~StubProcess() {
    Stop();
    if (region_) munmap(region_, STUB_REGION_SIZE);
}
void StubProcess::Stop() {
    if (pid <= 0) return;
    kill(pid, SIGKILL);
    pid_t reaped;
    do { reaped = waitpid(pid, &host_status, 0); } while (reaped < 0 && errno == EINTR);
    pid = -1;
}
bool StubProcess::Audit(std::string* error) const {
    const auto* boot = reinterpret_cast<const StubBoot*>(static_cast<char*>(region_) + STUB_BOOT_OFFSET);
    return AuditStub(pid, reinterpret_cast<uintptr_t>(region_), channel, boot->shared_memory_fd, boot->transfer_fd, error);
}
bool StubProcess::Start(const LoadedImage& img, Sentry* sentry, bool test_gate,
                        const StubContext* resume, std::string* error) {
    if (!resume) {
        if (!MapStack(error)) return false;
        AddressSpace& space = sentry->address_space();
        space.Init(img.image_end);
        for (const auto& segment : img.segments)
            space.Reserve(segment.start, segment.end, segment.prot, MAP_PRIVATE);
        space.Reserve(GuestWindow::stack_bottom(), GuestWindow::stack_top(),
                      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        space.Reserve(GuestWindow::signal_trampoline(), GuestWindow::signal_trampoline() + sysconf(_SC_PAGESIZE),
                      PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS);
    }
    if (!channel.Create(error)) return false;
    Runtime runtime;
    if (!PrepareRuntime(img, *sentry, channel, test_gate, &runtime, resume, error)) return false;
    region_ = runtime.region;
    runtime.region = MAP_FAILED;
    const uintptr_t base = reinterpret_cast<uintptr_t>(region_);
    pid = fork();
    if (pid < 0) { *error = std::string("fork: ") + strerror(errno); return false; }
    if (pid == 0) {
        auto enter = reinterpret_cast<void (*)(const StubBoot*)>(base + STUB_ENTRY_OFFSET);
        enter(reinterpret_cast<const StubBoot*>(base + STUB_BOOT_OFFSET));
        __builtin_trap();
    }
    sentry->set_guest_pid(pid);
    channel.ParentAfterFork();
    return true;
}
}
