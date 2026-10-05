#include "stub.h"
#include "metrics.h"
#include "session.h"
#include "stub_process.h"
#include "program.h"
#include "guest_layout.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/sched.h>
#include <linux/futex.h>
#include <map>
#include <sstream>
#include <set>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <termios.h>
#include <unistd.h>

namespace goblin {
namespace {
constexpr long kReplaced = LONG_MIN + 1;
uint64_t Now() {
    timespec time{}; clock_gettime(CLOCK_MONOTONIC, &time);
    return uint64_t(time.tv_sec) * 1000000000 + time.tv_nsec;
}
bool ResetScratch() {
    return GuestWindow::Reset();
}
struct SignalFrame { siginfo_t info; ucontext_t uc; };
struct Task {
    std::unique_ptr<Sentry> owned;
    Sentry* sentry = nullptr;
    std::shared_ptr<StubProcess> stub;
    StubContext context{};
    bool awaiting = true, applying = false, thread = false;
    std::optional<long> ready;
    uintptr_t robust_head = 0;
    uint64_t last_slice = 0;
    uint64_t started_ns = 0;
    bool dead = false, reaped = false, blocked = false, stopped = false;
    bool audited = true, stop_changed = false;
    int status = 0, vfork_parent = 0;
    bool vfork_wait = false;
    uint64_t pending = 0, deadline = 0;
    bool futex_woken = false;
    bool notified = false;
    siginfo_t info[65] = {};
    std::function<void(int)> on_exit;
    SyscallRequest request;
    std::optional<long> deferred;
    std::optional<uint64_t> restore_mask;
    stack_t altstack{nullptr, SS_DISABLE, 0};
};

struct TerminalBinding {
    std::shared_ptr<TerminalSession> session;
    std::shared_ptr<HostFile> master;
    std::shared_ptr<Terminal> tty;
    std::shared_ptr<Output> output;
    std::string input;
    int pid = 0, status = -1;
    bool creating = false, closed = false;
    uint64_t closed_at = 0;
};
class Kernel {
public:
    RunResult Run(const LoadedImage& image, Sentry* sentry, bool test_gate);
    ~Kernel() {
        for (const auto& task : tasks_) {
            auto proc = task->sentry->files().proc;
            proc->tasks = {}; proc->task = {};
            auto ns = task->sentry->files().unix_namespace;
            ns->roots = [] { return std::vector<std::shared_ptr<OpenFile>>{}; };
            ns->dirty = true; ns->Collect(); ns->roots = {};
            task->sentry->files().vfs->proc_enabled = false;
        }
    }
private:
    RuntimeControl* runtime_ = nullptr;
    std::vector<std::shared_ptr<TerminalBinding>> terminals_;
    void LaunchTerminal(const std::shared_ptr<TerminalSession>& session, Sentry& init);
    void PumpTerminals(Sentry& init);
    std::vector<std::unique_ptr<Task>> tasks_;
    int next_pid_ = 2;
    RunResult result_;
    uint64_t start_ = 0, handler_ns_ = 0;
    Task* Find(int pid);
    void Connect(Task& task);
    bool DescribeTask(int pid, unsigned detail, ProcTask* out);
    uint64_t MemoryUsed(const AddressSpace* exclude = nullptr) const;
    void Schedule();
    bool GroupAlive(int tgid) const;
    void FinishGroup(Task& task, int status, int signal = 0);
    void ExitFutexes(Task& task);
    std::map<StubProcess*, int> last_scheduled_;
    void Finish(Task& task, int status, int signal = 0, bool notify_parent = true);
    void Queue(Task& task, int signal, const siginfo_t& info);
    bool Deliver(Task& task, long result);
    bool HasSignal(const Task& task) const;
    void Reply(Task& task, long result);
    long Apply(Task& task, const AddressSpace::StubOp& operation);
    long Snapshot(Task& task, bool shared_vm = false);
    long Clone(Task& task, const SyscallRequest& request);
    long Exec(Task& task, const SyscallRequest& request);
    long Wait(Task& task, const SyscallRequest& request);
    std::optional<long> Dispatch(Task& task, const SyscallRequest& request);
};
Task* Kernel::Find(int pid) {
    for (auto& task : tasks_) if (!task->dead && !task->reaped && task->sentry->pid == pid) return task.get();
    for (auto& task : tasks_) if (!task->reaped && task->sentry->pid == pid) return task.get();
    return nullptr;
}
uint64_t Kernel::MemoryUsed(const AddressSpace* exclude) const {
    std::map<const AddressSpace*, uint64_t> sizes;
    for (const auto& task : tasks_) if (!task->dead) {
        const auto* space = &task->sentry->address_space();
        if (space != exclude) sizes[space] = std::max(sizes[space], task->sentry->reserved_memory());
    }
    uint64_t total = 0;
    for (const auto& item : sizes) total += item.second;
    return total;
}
void Kernel::Connect(Task& task) {
    if (!task.started_ns) task.started_ns = Now();
    task.sentry->dispatch = [this, &task](const SyscallRequest& req) { return Dispatch(task, req); };
    task.sentry->allow_memory = [this, &task](const AddressSpace& proposed) {
        uint64_t used = MemoryUsed(&task.sentry->address_space());
        uint64_t cap = task.sentry->limits->memory_bytes;
        return used <= cap && proposed.MappedBytes() <= cap - used;
    };
    task.sentry->files().output_limit = task.sentry->limits->output_bytes;
    auto& files = task.sentry->files();
    files.vfs->proc_enabled = true;
    files.proc->started_ns = start_;
    files.proc->memory_limit = task.sentry->limits->memory_bytes;
    files.proc->tasks = [this] {
        std::vector<std::pair<int, int>> ids;
        for (const auto& t : tasks_) if (!t->reaped) ids.emplace_back(t->sentry->pid, t->sentry->tgid);
        return ids;
    };
    files.proc->task = [this](int pid, unsigned detail, ProcTask* out) { return DescribeTask(pid, detail, out); };
    files.unix_namespace->roots = [this] {
        std::vector<std::shared_ptr<OpenFile>> roots;
        for (const auto& t : tasks_) if (!t->dead) {
            auto files = t->sentry->files().OpenFiles(); roots.insert(roots.end(), files.begin(), files.end());
        }
        return roots;
    };
}
bool Kernel::DescribeTask(int pid, unsigned detail, ProcTask* out) {
    Task* task = Find(pid); if (!task) return false;
    const auto& s = *task->sentry;
    out->pid = s.pid; out->tgid = s.tgid; out->ppid = s.ppid; out->group = s.group; out->session = s.session;
    out->credentials = s.credentials;
    out->state = task->dead ? 'Z' : task->stopped ? 'T' : task->blocked ? 'S' : 'R';
    out->bytes = task->dead ? 0 : s.address_space().MappedBytes();
    out->started = (task->started_ns - start_) / 10000000;
    out->blocked_signals = s.signal_mask;
    out->executable = s.files().executable;
    out->name = out->executable.substr(out->executable.rfind('/') + 1, 15);
    out->cwd = s.files().cwd;
    if (s.files().cwd_handle) s.files().vfs->PathFd(s.files().cwd_handle->fd, &out->cwd);
    out->threads = 0;
    for (const auto& t : tasks_) if (!t->dead && t->sentry->tgid == s.tgid) ++out->threads;
    out->threads = std::max(1, out->threads);
    if (auto tty = s.files().controlling_terminal()) { out->tty = (136 << 8) | tty->number; out->foreground = tty->foreground_group; }
    if (detail & ProcState::kFiles) {
        for (int fd = 0; fd < 256; ++fd) if (const auto* d = s.files().Get(fd)) out->files.emplace_back(fd, d->file);
    }
    if (detail & ProcState::kMaps) {
        for (const auto& v : s.address_space().vmas()) {
            char line[160];
            snprintf(line, sizeof(line), "%016lx-%016lx %c%c%c%c %08lx 00:00 0\n", v.start, v.end,
                v.prot & PROT_READ ? 'r' : '-', v.prot & PROT_WRITE ? 'w' : '-', v.prot & PROT_EXEC ? 'x' : '-',
                v.flags & MAP_SHARED ? 's' : 'p', v.file_offset);
            out->maps += line;
            if (out->maps.size() > (8u << 20)) break;
        }
    }
    if (detail & ProcState::kArguments) {
        if (!task->dead) {
            for (const auto& arg : s.arguments) { out->command += arg; out->command += '\0'; }
            for (const auto& var : s.environment) { out->environment += var; out->environment += '\0'; }
        }
    }
    return true;
}
void Kernel::Queue(Task& task, int signal, const siginfo_t& info) {
    if (task.dead || (runtime_ && task.sentry->pid == 1)) return;
    if (signal == SIGKILL) { FinishGroup(task, signal, signal); return; }
    if (signal == SIGCONT && task.stopped) {
        task.stopped = false; task.stop_changed = true; task.status = 0xffff;
        if (Task* parent = Find(task.sentry->ppid)) {
            siginfo_t continued{}; continued.si_signo = SIGCHLD; continued.si_code = CLD_CONTINUED;
            continued.si_pid = task.sentry->pid; continued.si_status = SIGCONT;
            Queue(*parent, SIGCHLD, continued);
        }
    }
    task.pending |= 1ULL << (signal - 1);
    task.info[signal] = info;
    if (task.awaiting && !task.applying && !task.blocked && !task.stopped && !task.vfork_wait && task.stub && HasSignal(task) && task.stub->channel.boot_stage() >= 5) {
        kill(task.stub->pid, SIGUSR1); task.notified = true;
    }
}
bool Kernel::HasSignal(const Task& task) const {
    uint64_t available = task.pending & ~task.sentry->signal_mask;
    for (int sig = 1; sig <= 64; ++sig) if (available & (1ULL << (sig - 1))) {
        auto action = task.sentry->actions()[sig];
        if (action.handler != 1 && (action.handler ||
            (sig != SIGCHLD && sig != SIGWINCH && sig != SIGURG && sig != SIGCONT))) return true;
    }
    return false;
}
void Kernel::Finish(Task& task, int status, int signal, bool notify_parent) {
    if (task.dead) return;
    ExitFutexes(task);
    task.dead = true; task.blocked = false; task.awaiting = false; task.ready.reset(); task.status = status;
    bool shared = false;
    for (const auto& other : tasks_) shared |= !other->dead && other->stub == task.stub;
    if (!shared && task.stub) task.stub->Stop();
    const bool group_dead = !GroupAlive(task.sentry->tgid);
    if (group_dead) {
        task.sentry->files().vfs->ReleaseLocks(task.sentry->tgid);
        if (task.sentry->tgid == task.sentry->session) {
            if (auto tty = task.sentry->files().controlling_terminal()) {
                int foreground = tty->foreground_group;
                tty->session = tty->foreground_group = 0;
                // APT reuses its PTY for successive dpkg sessions. Release
                // ownership when the session leader's last thread exits.
                for (auto& other : tasks_) if (!other->dead && other->sentry->group == foreground) {
                    for (int sig : {SIGHUP, SIGCONT}) {
                        siginfo_t info{}; info.si_signo = sig; info.si_code = SI_KERNEL;
                        Queue(*other, sig, info);
                    }
                }
            }
        }
    }
    task.sentry->ReleaseFiles();
    Task* leader = Find(task.sentry->tgid);
    if (group_dead && leader && leader->dead) {
        status = leader->status;
        signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    }
    if (group_dead && leader && leader->on_exit) {
        auto callback = std::move(leader->on_exit); callback(signal ? 128 + signal : WEXITSTATUS(status));
    }
    if (runtime_ && group_dead && task.sentry->ppid == 1) { task.reaped = true; if (leader) leader->reaped = true; }
    if (task.sentry->tgid == 1 && group_dead) {
        result_.exited = !signal;
        result_.status = signal ? 128 + signal : WEXITSTATUS(status);
        result_.fault_signal = signal;
        if (signal && result_.error.empty()) result_.error = std::string("guest terminated: ") + strsignal(signal);
    }
    if (Task* parent = group_dead && notify_parent ? Find(task.sentry->ppid) : nullptr) {
        siginfo_t info{}; info.si_signo = SIGCHLD;
        info.si_code = signal ? CLD_KILLED : CLD_EXITED;
        info.si_pid = task.sentry->tgid; info.si_uid = task.sentry->credentials.uid; info.si_status = signal ? signal : WEXITSTATUS(status);
        const auto& action = parent->sentry->actions()[SIGCHLD];
        if (action.handler == 1 || (action.flags & SA_NOCLDWAIT)) { task.reaped = true; if (leader) leader->reaped = true; }
        if (action.handler != 1) Queue(*parent, SIGCHLD, info);
    }
    if (task.vfork_parent) {
        if (Task* parent = Find(task.vfork_parent)) parent->vfork_wait = false;
        task.vfork_parent = 0;
    }
    if (task.thread) task.reaped = true;
    if (group_dead && notify_parent) for (auto& child : tasks_)
        if (child->sentry->ppid == task.sentry->tgid) { child->sentry->ppid = 1; if (runtime_ && child->dead) child->reaped = true; }
}
bool Kernel::Deliver(Task& task, long result) {
    auto& s = *task.sentry;
    uint64_t available = task.pending & ~s.signal_mask;
    for (int signal = 1; signal <= 64; ++signal) {
        const uint64_t bit = 1ULL << (signal - 1);
        if (!(available & bit)) continue;
        task.pending &= ~bit;
        auto action = s.actions()[signal];
        if (action.handler == 1) continue;
        if (!action.handler) {
            if (signal == SIGCHLD || signal == SIGWINCH || signal == SIGURG || signal == SIGCONT) continue;
            if (signal == SIGSTOP || signal == SIGTSTP || signal == SIGTTIN || signal == SIGTTOU) {
                if (!task.blocked || result != -EINTR) task.deferred = result;
                task.stopped = true; task.blocked = true; task.stop_changed = true;
                task.status = (signal << 8) | 0x7f;
                if (Task* parent = Find(s.ppid)) {
                    siginfo_t info{}; info.si_signo = SIGCHLD; info.si_code = CLD_STOPPED;
                    info.si_pid = s.pid; info.si_status = signal; Queue(*parent, SIGCHLD, info);
                }
                return false;
            }
            FinishGroup(task, signal, signal); return false;
        }
        StubContext context = task.context;
        if (task.request.nr >= 0 && task.request.nr != 139) context.machine.regs[0] = result;
        // Restart only operations whose arguments can safely be replayed.
        if (result == -EINTR && (action.flags & SA_RESTART) &&
            (task.request.nr == 63 || task.request.nr == 64 || task.request.nr == 260)) {
            context.machine.pc -= 4;
            for (int i = 0; i < 6; ++i) context.machine.regs[i] = task.request.args[i];
            context.machine.regs[8] = task.request.nr;
        }
        SignalFrame frame{};
        frame.info = task.info[signal];
        frame.uc.uc_mcontext = context.machine;
        frame.uc.uc_stack = task.altstack;
        uint64_t saved_mask = task.restore_mask.value_or(s.signal_mask);
        memcpy(&frame.uc.uc_sigmask, &saved_mask, 8);
        uintptr_t top = context.machine.sp;
        if ((action.flags & SA_ONSTACK) && !(task.altstack.ss_flags & SS_DISABLE)) {
            uintptr_t lo = reinterpret_cast<uintptr_t>(task.altstack.ss_sp);
            if (top < lo || top >= lo + task.altstack.ss_size) top = lo + task.altstack.ss_size;
        }
        uintptr_t address = (top - sizeof(frame)) & ~uintptr_t(15);
        if (top < sizeof(frame) || !s.memory().Write(address, &frame, sizeof(frame))) {
            FinishGroup(task, SIGSEGV, SIGSEGV); return false;
        }
        context.machine.sp = address;
        context.machine.pc = action.handler;
        context.machine.regs[0] = signal;
        context.machine.regs[1] = address;
        context.machine.regs[2] = address + offsetof(SignalFrame, uc);
        context.machine.regs[30] = (action.flags & 0x04000000) ? action.restorer : GuestWindow::signal_trampoline();
        s.signal_mask |= action.mask;
        if (!(action.flags & SA_NODEFER)) s.signal_mask |= bit;
        s.signal_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
        if (action.flags & SA_RESETHAND) s.actions()[signal] = {};
        task.context = context;
        task.restore_mask.reset();
        break;
    }
    return true;
}
bool Kernel::GroupAlive(int tgid) const {
    for (const auto& t : tasks_) if (!t->dead && t->sentry->tgid == tgid) return true;
    return false;
}
void Kernel::FinishGroup(Task& task, int status, int signal) {
    const int group = task.sentry->tgid;
    if (Task* leader = Find(group)) leader->status = status;
    for (auto& other : tasks_) if (!other->dead && other->sentry->tgid == group) Finish(*other, status, signal);
}
void Kernel::ExitFutexes(Task& task) {
    auto wake = [&](uintptr_t address) {
        for (auto& other : tasks_) if (other.get() != &task && !other->dead && other->blocked &&
            other->request.nr == 98 && other->request.args[0] == address &&
            &other->sentry->address_space() == &task.sentry->address_space()) other->futex_woken = true;
    };
    auto recover = [&](uintptr_t address) {
        uint32_t value;
        if (!(address & 3) && task.sentry->memory().Read(address, &value, 4) &&
            (value & FUTEX_TID_MASK) == static_cast<unsigned>(task.sentry->pid)) {
            value = (value & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
            if (task.sentry->memory().Write(address, &value, 4)) wake(address);
        }
    };
    struct RobustHead { uint64_t next; int64_t offset; uint64_t pending; } head{};
    if (task.robust_head && task.sentry->memory().Read(task.robust_head, &head, sizeof(head))) {
        uintptr_t node = head.next;
        for (unsigned i = 0; i < 2048 && node && node != task.robust_head; ++i) {
            if (node & 1) break; // PI recovery needs the PI protocol.
            uint64_t next;
            if (!task.sentry->memory().Read(node, &next, 8)) break;
            if (node != head.pending) recover(node + head.offset);
            node = next;
        }
        if (head.pending && !(head.pending & 1)) recover(head.pending + head.offset);
    }
    bool shared = false;
    for (const auto& other : tasks_) shared |= other.get() != &task && !other->dead && other->stub == task.stub;
    if (shared && task.sentry->clear_tid) {
        uint32_t zero = 0;
        if (task.sentry->memory().Write(task.sentry->clear_tid, &zero, 4)) wake(task.sentry->clear_tid);
    }
    task.sentry->clear_tid = 0;
}
void Kernel::Reply(Task& task, long result) {
    if (task.stopped || task.vfork_wait) { task.deferred = result; task.blocked = true; return; }
    const long nr = task.request.nr;
    const bool writes = nr == 64 || nr == 66 || nr == 68 ||
        (nr == 206 && !(task.request.args[3] & MSG_NOSIGNAL)) ||
        (nr == 211 && !(task.request.args[2] & MSG_NOSIGNAL)) ||
        (nr == 269 && !(task.request.args[3] & MSG_NOSIGNAL));
    // rt_sigreturn restores the interrupted write's -EPIPE in x0. It must
    // not generate another SIGPIPE on every return from the handler.
    if (result == -EPIPE && writes) {
        siginfo_t info{}; info.si_signo = SIGPIPE; info.si_code = SI_USER;
        task.pending |= 1ULL << (SIGPIPE - 1); task.info[SIGPIPE] = info;
    }
    task.ready = result;
}
void Kernel::Schedule() {
    for (auto& candidate : tasks_) {
        if (candidate->dead || !candidate->ready || candidate->stopped || candidate->vfork_wait) continue;
        bool busy = false;
        for (const auto& t : tasks_) busy |= !t->dead && t->stub == candidate->stub && t->awaiting;
        if (busy) continue;
        Task* selected = candidate.get();
        const int last = last_scheduled_[selected->stub.get()];
        for (auto& t : tasks_) if (!t->dead && t->ready && !t->stopped && !t->vfork_wait && t->stub == selected->stub) {
            if ((t->sentry->pid > last && (selected->sentry->pid <= last || t->sentry->pid < selected->sentry->pid)) ||
                (t->sentry->pid <= last && selected->sentry->pid <= last && t->sentry->pid < selected->sentry->pid)) selected = t.get();
        }
        Task& task = *selected;
        long result = *task.ready; task.ready.reset();
        if (task.request.nr >= 0 && task.request.nr != 139) task.context.machine.regs[0] = result;
        if (!Deliver(task, result)) continue;
        task.blocked = false; task.deadline = 0;
        if (task.restore_mask) { task.sentry->signal_mask = *task.restore_mask; task.restore_mask.reset(); }
        task.stub->channel.SetContext(task.context);
        task.stub->channel.Reply(result, {});
        task.awaiting = true; task.last_slice = Now();
        last_scheduled_[task.stub.get()] = task.sentry->pid;
    }
}
long Kernel::Apply(Task& task, const AddressSpace::StubOp& operation) {
    task.stub->channel.Reply(0, operation);
    const uint64_t deadline = Now() + 5000000000ULL;
    while (Now() < deadline) {
        SyscallRequest ignored;
        auto event = task.stub->channel.Wait(task.stub->pid, &ignored);
        if (event == SyscallChannel::Event::kApplied) {
            long result = task.stub->channel.applied_result();
            long expected = (operation.kind == AddressSpace::StubOp::kMap || operation.kind == AddressSpace::StubOp::kMapFile) ? operation.addr : 0;
            return result < 0 ? result : result == expected ? 0 : -EIO;
        }
        if (event != SyscallChannel::Event::kTimeout) return -EIO;
    }
    return -ETIMEDOUT;
}
long Kernel::Snapshot(Task& task, bool shared_vm) {
    size_t total = 0;
    for (const auto& v : task.sentry->address_space().vmas()) {
        if (!v.forkable && !shared_vm) continue;
        if ((v.flags & MAP_TYPE) == MAP_SHARED && !v.shared_backed && !v.file) return -EOPNOTSUPP;
        total += v.end - v.start;
        if (total > (256u << 20)) return -ENOMEM;
    }
    if (!ResetScratch()) return -ENOMEM;
    std::vector<uint8_t> buffer(65536);
    for (const auto& v : task.sentry->address_space().vmas()) {
        if (!v.forkable && !shared_vm) continue;
        if (v.file && ((v.flags & MAP_TYPE) == MAP_SHARED || ((v.prot & PROT_EXEC) && !(v.prot & PROT_WRITE)))) {
            if (mmap(reinterpret_cast<void*>(GuestWindow::BrokerAddress(v.start)), v.end - v.start, v.prot,
                     (v.flags & MAP_TYPE) | MAP_FIXED, v.file->fd, v.file_offset) == MAP_FAILED) return -errno;
            continue;
        }
        if (v.shared_backed) {
            if (mmap(reinterpret_cast<void*>(GuestWindow::BrokerAddress(v.start)), v.end - v.start, v.prot, MAP_SHARED | MAP_FIXED,
                     task.sentry->shared_memory->file->fd, v.backing_offset) == MAP_FAILED) return -errno;
            continue;
        }
        bool temporary = !(v.prot & PROT_READ);
        if (temporary) {
            long rc = Apply(task, {AddressSpace::StubOp::kProtect, v.start, v.end - v.start, v.prot | PROT_READ, 0});
            if (rc < 0) return rc;
        }
        long rc = 0;
        uintptr_t readable_end = v.end;
        if (v.file) {
            struct stat st{};
            if (fstat(v.file->fd, &st) < 0) rc = -errno;
            const size_t ps = sysconf(_SC_PAGESIZE);
            uint64_t end = (uint64_t(st.st_size) + ps - 1) & ~(ps - 1);
            readable_end = v.start + std::min<uint64_t>(v.end - v.start, end > v.file_offset ? end - v.file_offset : 0);
        }
        if (rc == 0 && mmap(reinterpret_cast<void*>(GuestWindow::BrokerAddress(v.start)), v.end - v.start, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_FIXED | (v.file ? 0 : MAP_ANONYMOUS),
                 v.file ? v.file->fd : -1, v.file_offset) == MAP_FAILED) rc = -errno;
        for (uintptr_t p = v.start; rc == 0 && p < readable_end; p += buffer.size()) {
            size_t bytes = std::min<uintptr_t>(buffer.size(), readable_end - p);
            if (!task.sentry->memory().Read(p, buffer.data(), bytes)) rc = -EFAULT;
            else memcpy(reinterpret_cast<void*>(GuestWindow::BrokerAddress(p)), buffer.data(), bytes);
        }
        if (temporary) {
            long restored = Apply(task, {AddressSpace::StubOp::kProtect, v.start, v.end - v.start, v.prot, 0});
            if (restored < 0) return restored;
        }
        if (rc < 0) return rc;
        char* staging = reinterpret_cast<char*>(GuestWindow::BrokerAddress(v.start));
        if (v.prot & PROT_EXEC) __builtin___clear_cache(staging, staging + v.end - v.start);
        if (mprotect(staging, v.end - v.start, v.prot) < 0) return -errno;
    }
    return 0;
}
long Kernel::Clone(Task& task, const SyscallRequest& req) {
    const auto& a = req.args;
    const unsigned long flags = a[0];
    constexpr unsigned long supported = 255 | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID |
        CLONE_PARENT_SETTID | CLONE_SETTLS | CLONE_VM | CLONE_VFORK |
        CLONE_THREAD | CLONE_SIGHAND | CLONE_FS | CLONE_FILES | CLONE_SYSVSEM;
    if (flags & ~supported) return -ENOSYS;
    const bool thread = flags & CLONE_THREAD;
    if (thread && ((flags & (CLONE_VM | CLONE_SIGHAND | CLONE_FS | CLONE_FILES)) !=
        (CLONE_VM | CLONE_SIGHAND | CLONE_FS | CLONE_FILES) || (flags & (255 | CLONE_VFORK)) || !a[1])) return -EINVAL;
    if (!thread && (flags & 255) != SIGCHLD) return -EINVAL;
    if (!thread && (flags & (CLONE_SIGHAND | CLONE_FS | CLONE_FILES))) return -ENOSYS;
    size_t live = 0; for (const auto& t : tasks_) live += !t->reaped;
    if (live >= task.sentry->limits->tasks) return -EAGAIN;
    uint32_t original;
    if ((flags & CLONE_PARENT_SETTID) && (!task.sentry->memory().Read(a[2], &original, 4) ||
        !task.sentry->memory().Write(a[2], &original, 4))) return -EFAULT;
    if ((flags & CLONE_CHILD_SETTID) && (!task.sentry->memory().Read(a[4], &original, 4) ||
        !task.sentry->memory().Write(a[4], &original, 4))) return -EFAULT;
    StubContext context = task.context;
    context.machine.regs[0] = 0;
    if (a[1]) context.machine.sp = a[1];
    if (flags & CLONE_SETTLS) context.tls = a[3];
    if (!(flags & CLONE_VM)) {
        uint64_t used = MemoryUsed(), cap = task.sentry->limits->memory_bytes;
        if (used > cap || task.sentry->reserved_memory() > cap - used) return -ENOMEM;
    }
    long rc = (flags & CLONE_VM) ? 0 : Snapshot(task); if (rc < 0) return rc;
    auto child = std::make_unique<Task>();
    child->owned = task.sentry->ForkState(); child->sentry = child->owned.get();
    if (thread) child->sentry->ShareThreadState(*task.sentry);
    else if (flags & CLONE_VM) child->sentry->ShareMemory(*task.sentry);
    if (!(flags & CLONE_VM)) child->sentry->address_space().AfterFork();
    child->sentry->pid = next_pid_++; child->sentry->ppid = thread ? task.sentry->ppid : task.sentry->tgid;
    child->sentry->tgid = thread ? task.sentry->tgid : child->sentry->pid;
    child->thread = thread;
    child->altstack = (flags & CLONE_VM) && !(flags & CLONE_VFORK) ? stack_t{nullptr, SS_DISABLE, 0} : task.altstack;
    if (flags & CLONE_CHILD_CLEARTID) child->sentry->clear_tid = a[4];
    if (flags & CLONE_CHILD_SETTID) {
        GuestMemory local;
        auto& destination = (flags & CLONE_VM) ? task.sentry->memory() : local;
        if (!destination.Write(a[4], &child->sentry->pid, 4)) return -EFAULT;
    }
    if (flags & CLONE_VM) {
        child->stub = task.stub; child->sentry->set_guest_pid(task.stub->pid);
        child->context = context; child->awaiting = false; child->ready = 0;
        child->request.nr = -1;
    } else {
        child->stub = std::make_shared<StubProcess>();
        LoadedImage placeholder; std::string error;
        if (!child->stub->Start(placeholder, child->sentry, false, &context, &error)) return -EAGAIN;
    }
    if (flags & CLONE_PARENT_SETTID) task.sentry->memory().Write(a[2], &child->sentry->pid, 4);
    if (flags & CLONE_VFORK) {
        child->vfork_parent = task.sentry->pid;
        task.vfork_wait = true;
    }
    int pid = child->sentry->pid;
    Connect(*child); tasks_.push_back(std::move(child));
    return pid;
}
long Kernel::Exec(Task& task, const SyscallRequest& req) {
    std::string path;
    int rc = task.sentry->files().Path(AT_FDCWD, req.args[0], task.sentry->memory(), &path);
    if (rc < 0) return rc;
    std::vector<std::string> argv, envp;
    size_t bytes = 0;
    auto strings = [&](uintptr_t address, std::vector<std::string>* out) -> int {
        if (!address) return 0;
        for (size_t i = 0; i < 1024; ++i) {
            uintptr_t p;
            if (!task.sentry->memory().Read(address + i * 8, &p, 8)) return -EFAULT;
            if (!p) return 0;
            std::string value;
            if (!task.sentry->memory().ReadString(p, &value, 131072)) return -EFAULT;
            bytes += value.size() + 1;
            if (bytes > 131072) return -E2BIG;
            out->push_back(std::move(value));
        }
        return -E2BIG;
    };
    rc = strings(req.args[1], &argv); if (rc < 0) return rc;
    rc = strings(req.args[2], &envp); if (rc < 0) return rc;
    if (argv.empty()) argv.emplace_back("");
    struct stat st{};
    rc = task.sentry->files().vfs->Stat(path, &st);
    if (rc < 0) return rc;
    if (!S_ISREG(st.st_mode) || CheckPermission(st, X_OK)) return -EACCES;
    const std::string execfn = path;
    for (int depth = 0; ; ++depth) {
        std::vector<uint8_t> header; std::string error;
        if (!task.sentry->files().vfs->ReadFile(path, &header, &error)) return -ENOEXEC;
        if (header.size() < 2 || header[0] != '#' || header[1] != '!') break;
        if (depth >= 4) return -ELOOP;
        size_t end = 2;
        while (end < header.size() && end < 256 && header[end] != '\n') ++end;
        if (end == 256) return -ENOEXEC;
        std::string line(reinterpret_cast<char*>(header.data() + 2), end - 2);
        size_t begin = line.find_first_not_of(" \t");
        if (begin == std::string::npos) return -ENOEXEC;
        line = line.substr(begin, line.find_last_not_of(" \t") - begin + 1);
        size_t split = line.find_first_of(" \t");
        std::string interpreter = line.substr(0, split);
        std::vector<std::string> arguments{interpreter};
        if (split != std::string::npos) {
            begin = line.find_first_not_of(" \t", split);
            if (begin != std::string::npos) arguments.push_back(line.substr(begin));
        }
        arguments.push_back(path);
        arguments.insert(arguments.end(), argv.begin() + 1, argv.end());
        argv = std::move(arguments);
        path = interpreter[0] == '/' ? interpreter : task.sentry->files().cwd + "/" + interpreter;
        rc = task.sentry->files().vfs->Stat(path, &st);
        if (rc < 0) return rc;
        if (!S_ISREG(st.st_mode) || CheckPermission(st, X_OK)) return -EACCES;
    }
    if (!ResetScratch()) return -ENOMEM;
    LoadedImage image; std::string error;
    bool retained_mm = false;
    for (const auto& other : tasks_) if (!other->dead && other->sentry->tgid != task.sentry->tgid &&
        &other->sentry->address_space() == &task.sentry->address_space()) retained_mm = true;
    uint64_t reserved = MemoryUsed(retained_mm ? nullptr : &task.sentry->address_space());
    if (reserved >= task.sentry->limits->memory_bytes) return -ENOMEM;
    if (!LoadProgram(*task.sentry->files().vfs, path, &image, &error, task.sentry->limits->memory_bytes - reserved)) return -ENOEXEC;
    auto replacement = task.sentry->ForkState(false);
    replacement->arguments = std::move(argv); replacement->environment = std::move(envp);
    replacement->files().executable = execfn;
    replacement->ResetAfterExec();
    // Only the ELF's metadata applies: script set-ID bits were discarded when
    // resolving its interpreter. These are virtual guest IDs, never host IDs.
    if (!replacement->credentials.no_new_privs) {
        if (st.st_mode & S_ISUID) replacement->credentials.euid = st.st_uid;
        if ((st.st_mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) replacement->credentials.egid = st.st_gid;
    }
    replacement->credentials.suid = replacement->credentials.fsuid = replacement->credentials.euid;
    replacement->credentials.sgid = replacement->credentials.fsgid = replacement->credentials.egid;
    auto stub = std::make_shared<StubProcess>();
    if (!stub->Start(image, replacement.get(), false, nullptr, &error)) return -EAGAIN;
    bool old_survives = false;
    for (const auto& other : tasks_) if (!other->dead && other->sentry->tgid != task.sentry->tgid &&
        &other->sentry->address_space() == &task.sentry->address_space()) old_survives = true;
    uint64_t used = MemoryUsed(old_survives ? nullptr : &task.sentry->address_space());
    if (used > replacement->limits->memory_bytes || replacement->reserved_memory() > replacement->limits->memory_bytes - used) {
        stub->Stop(); return -ENOMEM;
    }
    for (auto& other : tasks_) if (other.get() != &task && !other->dead && other->sentry->tgid == task.sentry->tgid) {
        Finish(*other, 0, 0, false); other->reaped = true;
    }
    replacement->pid = replacement->tgid;
    task.thread = false; task.awaiting = true; task.ready.reset(); task.robust_head = 0;
    task.stub = std::move(stub);
    *task.sentry = std::move(*replacement);
    task.altstack = {nullptr, SS_DISABLE, 0}; task.deadline = 0;
    Connect(task);
    if (task.vfork_parent) {
        if (Task* parent = Find(task.vfork_parent)) parent->vfork_wait = false;
        task.vfork_parent = 0;
    }
    return kReplaced;
}
long Kernel::Wait(Task& task, const SyscallRequest& req) {
    const int wanted = static_cast<int>(req.args[0]);
    if (req.args[2] & ~(WNOHANG | WUNTRACED | WCONTINUED)) return -EINVAL;
    bool any = false;
    for (auto& child : tasks_) {
        if (child->thread || child->reaped || child->sentry->ppid != task.sentry->tgid) continue;
        if (wanted > 0 && child->sentry->pid != wanted) continue;
        if (wanted == 0 && child->sentry->group != task.sentry->group) continue;
        if (wanted < -1 && child->sentry->group != -static_cast<int64_t>(wanted)) continue;
        any = true;
        if (child->dead && GroupAlive(child->sentry->tgid)) continue;
        if (!child->dead && !(child->stop_changed &&
            ((child->status == 0xffff && (req.args[2] & WCONTINUED)) ||
             (child->status != 0xffff && (req.args[2] & WUNTRACED))))) continue;
        if (req.args[1] && !task.sentry->memory().Write(req.args[1], &child->status, 4)) return -EFAULT;
        rusage usage{};
        if (req.args[3] && !task.sentry->memory().Write(req.args[3], &usage, sizeof(usage))) return -EFAULT;
        if (child->dead) child->reaped = true;
        child->stop_changed = false;
        return child->sentry->pid;
    }
    return !any ? -ECHILD : (req.args[2] & WNOHANG) ? 0 : kBlocked;
}

std::optional<long> Kernel::Dispatch(Task& task, const SyscallRequest& req) {
    auto& s = *task.sentry;
    const auto& a = req.args;
    auto temporary_mask = [&](uintptr_t pointer, size_t size) -> long {
        if (!pointer) return 0;
        if (size != 8) return -EINVAL;
        if (task.restore_mask) return 0;
        uint64_t mask;
        if (!s.memory().Read(pointer, &mask, 8)) return -EFAULT;
        task.restore_mask = s.signal_mask;
        s.signal_mask = mask & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
        return 0;
    };
    switch (req.nr) {
        case 436: { // close_range: touches virtual descriptors only.
            const unsigned first = a[0], last = a[1], flags = a[2];
            if (first > last || (flags & ~6u)) return -EINVAL;
            if (flags & 2) s.UnshareFiles();
            s.files().lock_owner = s.tgid;
            s.files().CloseRange(first, last, flags & 4);
            return 0;
        }
        case 93: Finish(task, (a[0] & 255) << 8); return 0;
        case 94: FinishGroup(task, (a[0] & 255) << 8); return 0;
        case 99:
            if (a[1] != 24) return -EINVAL;
            task.robust_head = a[0]; return 0;
        case 100: {
            Task* target = a[0] ? Find(a[0]) : &task;
            if (!target || target->dead) return -ESRCH;
            uint64_t size = 24;
            return s.memory().Write(a[1], &target->robust_head, 8) && s.memory().Write(a[2], &size, 8) ? 0 : -EFAULT;
        }
        case 29: {
            const auto* descriptor = s.files().Get(a[0]);
            if (!descriptor || !descriptor->file->terminal) return {};
            auto terminal = descriptor->file->terminal;
            if (a[1] == TIOCSCTTY) {
                if (s.tgid != s.session) return -EPERM;
                if (terminal->session != s.session) {
                    if (s.files().controlling_terminal()) return -EPERM;
                    if (terminal->session && (a[2] != 1 || s.credentials.euid)) return -EPERM;
                    if ((descriptor->file->flags & O_ACCMODE) == O_WRONLY && s.credentials.euid) return -EPERM;
                    // sudo's monitor creates a session before acquiring its
                    // slave. Both IDs must follow that new session, rather
                    // than retaining those of the shell that opened ptmx.
                    terminal->session = s.session;
                    terminal->foreground_group = s.group;
                }
            }
            if (a[1] == TIOCSPGRP) {
                int group;
                if (!s.memory().Read(a[2], &group, 4)) return -EFAULT;
                if (group < 0) return -EINVAL;
                if (s.files().controlling_terminal() != terminal) return -ENOTTY;
                bool found = false;
                for (const auto& other : tasks_) found |= !other->dead && other->sentry->group == group && other->sentry->session == s.session;
                if (!found || s.session != terminal->session) return -EPERM;
            }
            auto value = s.files().Handle(req, s.memory());
            if (value && *value == 0 && a[1] == TIOCSWINSZ) {
                siginfo_t info{}; info.si_signo = SIGWINCH; info.si_code = SI_KERNEL;
                for (auto& other : tasks_) if (other->sentry->group == terminal->foreground_group) Queue(*other, SIGWINCH, info);
            }
            return value;
        }
        case 63: {
            const auto* descriptor = s.files().Get(a[0]);
            if (descriptor && descriptor->file->terminal && !descriptor->file->pty_master) {
                auto terminal = descriptor->file->terminal;
                if (terminal->session == s.session && terminal->foreground_group != s.group) {
                    if ((s.signal_mask & (1ULL << (SIGTTIN - 1))) || s.actions()[SIGTTIN].handler == 1) return -EIO;
                    siginfo_t info{}; info.si_signo = SIGTTIN; info.si_code = SI_KERNEL;
                    task.pending |= 1ULL << (SIGTTIN - 1); task.info[SIGTTIN] = info;
                    return -EINTR;
                }
            }
            return {};
        }
        case 72: { // pselect6 with the descriptor sets kept in guest memory.
            if (a[0] > 256) return -EINVAL;
            if (a[5]) {
                struct { uint64_t address, size; } mask{};
                if (!s.memory().Read(a[5], &mask, sizeof(mask))) return -EFAULT;
                long rc = temporary_mask(mask.address, mask.size); if (rc < 0) return rc;
            }
            const size_t words = (a[0] + 63) / 64;
            uint64_t sets[3][4] = {}, returned[3][4] = {};
            for (int k = 0; k < 3; ++k) if (a[k + 1] && !s.memory().Read(a[k + 1], sets[k], words * 8)) return -EFAULT;
            std::vector<pollfd> descriptors;
            std::vector<int> numbers;
            for (unsigned fd = 0; fd < a[0]; ++fd) {
                short events = 0;
                if (sets[0][fd / 64] & (1ULL << (fd % 64))) events |= POLLIN;
                if (sets[1][fd / 64] & (1ULL << (fd % 64))) events |= POLLOUT;
                if (sets[2][fd / 64] & (1ULL << (fd % 64))) events |= POLLPRI;
                if (!events) continue;
                if (!s.files().Get(fd)) return -EBADF;
                descriptors.push_back({s.files().HostFd(fd), events, 0}); numbers.push_back(fd);
            }
            if (poll(descriptors.data(), descriptors.size(), 0) < 0) return -errno;
            int count = 0;
            for (size_t i = 0; i < descriptors.size(); ++i) {
                auto p = descriptors[i];
                if (p.fd < 0) p.revents = s.files().VirtualEvents(numbers[i], p.events);
                const int fd = numbers[i];
                const short masks[] = {POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLPRI};
                for (int k = 0; k < 3; ++k) if ((p.revents & masks[k]) && (sets[k][fd / 64] & (1ULL << (fd % 64)))) {
                    returned[k][fd / 64] |= 1ULL << (fd % 64); ++count;
                }
            }
            if (a[4] && !task.deadline) {
                timespec timeout{};
                if (!s.memory().Read(a[4], &timeout, sizeof(timeout))) return -EFAULT;
                if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000 || timeout.tv_sec > 86400) return -EINVAL;
                task.deadline = Now() + uint64_t(timeout.tv_sec) * 1000000000 + timeout.tv_nsec;
            }
            if (!count && (!task.deadline || Now() < task.deadline)) return kBlocked;
            for (int k = 0; k < 3; ++k) if (a[k + 1] && !s.memory().Write(a[k + 1], returned[k], words * 8)) return -EFAULT;
            if (a[4]) { timespec zero{}; if (!s.memory().Write(a[4], &zero, sizeof(zero))) return -EFAULT; }
            return count;
        }
        case 98: {
            const unsigned operation = a[1] & FUTEX_CMD_MASK;
            if (a[0] & 3) return -EINVAL;
            uint32_t value;
            if (!s.memory().Read(a[0], &value, 4)) return -EFAULT;
            if (a[1] & ~(FUTEX_CMD_MASK | FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)) return -EINVAL;
            const uint32_t mask = (operation == FUTEX_WAIT_BITSET || operation == FUTEX_WAKE_BITSET) ? a[5] : UINT32_MAX;
            if (!mask) return -EINVAL;
            auto key = [](const Task& t, const SyscallRequest& request) {
                std::array<uint64_t, 4> result{2, reinterpret_cast<uintptr_t>(&t.sentry->address_space()), 0, request.args[0]};
                if (!(request.args[1] & FUTEX_PRIVATE_FLAG)) {
                    const auto* v = t.sentry->address_space().Lookup(request.args[0]);
                    if (v && v->shared_backed) result = {0, 0, 0, v->backing_offset + request.args[0] - v->start};
                    else if (v && v->file && (v->flags & MAP_TYPE) == MAP_SHARED) {
                        struct stat st{};
                        if (fstat(v->file->fd, &st) == 0) result = {1, st.st_dev, st.st_ino, v->file_offset + request.args[0] - v->start};
                    }
                }
                return result;
            };
            if (operation == FUTEX_WAKE || operation == FUTEX_WAKE_BITSET) {
                if (static_cast<int32_t>(a[2]) < 0) return -EINVAL;
                int woken = 0;
                for (auto& other : tasks_) {
                    if (woken >= static_cast<int32_t>(a[2])) break;
                    if (other->dead || !other->blocked || other->futex_woken || other->request.nr != 98) continue;
                    unsigned command = other->request.args[1] & FUTEX_CMD_MASK;
                    if (command != FUTEX_WAIT && command != FUTEX_WAIT_BITSET) continue;
                    uint32_t wanted = command == FUTEX_WAIT ? UINT32_MAX : other->request.args[5];
                    if ((mask & wanted) && key(*other, other->request) == key(task, req)) {
                        other->futex_woken = true; ++woken;
                    }
                }
                return woken;
            }
            if (operation != FUTEX_WAIT && operation != FUTEX_WAIT_BITSET) return -ENOSYS;
            if (task.futex_woken) { task.futex_woken = false; return 0; }
            // The value is compared atomically with registering the waiter:
            // only this broker dispatches guest wake operations.
            if (!task.blocked && value != static_cast<uint32_t>(a[2])) return -EAGAIN;
            if (a[3] && !task.deadline) {
                timespec timeout{};
                if (!s.memory().Read(a[3], &timeout, sizeof(timeout))) return -EFAULT;
                if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000 ||
                    uint64_t(timeout.tv_sec) > UINT64_MAX / 1000000000) return -EINVAL;
                uint64_t duration = uint64_t(timeout.tv_sec) * 1000000000 + timeout.tv_nsec;
                if (operation == FUTEX_WAIT_BITSET) {
                    timespec current{};
                    clock_gettime(a[1] & FUTEX_CLOCK_REALTIME ? CLOCK_REALTIME : CLOCK_MONOTONIC, &current);
                    uint64_t epoch = uint64_t(current.tv_sec) * 1000000000 + current.tv_nsec;
                    duration = duration > epoch ? duration - epoch : 0;
                } else if (a[1] & FUTEX_CLOCK_REALTIME) return -ENOSYS;
                if (duration > UINT64_MAX - Now()) return -EINVAL;
                task.deadline = Now() + duration;
            }
            return task.deadline && Now() >= task.deadline ? -ETIMEDOUT : kBlocked;
        }
        case -1: return 0; // host notification; preserve interrupted x0.
        case -2: {
            int signal = task.stub->channel.fault_signal();
            if (signal <= 0 || signal > 64) return -EINVAL;
            siginfo_t info{}; info.si_signo = signal; info.si_code = task.stub->channel.fault_code();
            info.si_addr = reinterpret_cast<void*>(task.stub->channel.fault_addr());
            task.pending |= 1ULL << (signal - 1); task.info[signal] = info;
            // A blocked or ignored synchronous fault cannot make progress.
            if ((s.signal_mask & (1ULL << (signal - 1))) || s.actions()[signal].handler == 1) FinishGroup(task, signal, signal);
            return 0;
        }
        case 220: return Clone(task, req);
        case 221: return Exec(task, req);
        case 260: return Wait(task, req);
        case 129: case 130: case 131: { // kill, tkill, tgkill; only virtual IDs.
            int target = a[0], signal = a[1];
            if (req.nr == 131) {
                Task* addressed = Find(a[1]);
                if (!addressed || addressed->sentry->tgid != static_cast<int>(a[0])) return -ESRCH;
                target = a[1]; signal = a[2];
            }
            if (signal < 0 || signal > 64) return -EINVAL;
            bool found = false, permitted = false;
            for (auto& other : tasks_) {
                if (other->dead || other->reaped) continue;
                bool match = target > 0 ? (req.nr == 129 ? other->sentry->tgid == target : other->sentry->pid == target) :
                    target == 0 ? other->sentry->group == s.group :
                    target == -1 ? other->sentry->pid != s.pid : other->sentry->group == -static_cast<int64_t>(target);
                if (!match) continue;
                found = true;
                const auto& c = s.credentials; const auto& target_credentials = other->sentry->credentials;
                if (c.euid && c.uid != target_credentials.uid && c.uid != target_credentials.suid &&
                    c.euid != target_credentials.uid && c.euid != target_credentials.suid &&
                    !(signal == SIGCONT && s.session == other->sentry->session)) continue;
                permitted = true;
                if (signal) {
                    siginfo_t info{}; info.si_signo = signal; info.si_code = SI_USER; info.si_pid = s.pid; info.si_uid = s.credentials.uid;
                    Queue(*other, signal, info);
                }
                if (target > 0) break;
            }
            return permitted ? 0 : found ? -EPERM : -ESRCH;
        }
        case 132: { // sigaltstack
            stack_t proposed{};
            if (a[0] && !s.memory().Read(a[0], &proposed, sizeof(proposed))) return -EFAULT;
            uintptr_t sp = task.context.machine.sp;
            uintptr_t start = reinterpret_cast<uintptr_t>(task.altstack.ss_sp);
            bool onstack = !(task.altstack.ss_flags & SS_DISABLE) && sp >= start && sp - start < task.altstack.ss_size;
            stack_t old = task.altstack; if (onstack) old.ss_flags |= SS_ONSTACK;
            if (a[1] && !s.memory().Write(a[1], &old, sizeof(old))) return -EFAULT;
            if (a[0]) {
                if (onstack) return -EPERM;
                if (proposed.ss_flags & ~SS_DISABLE) return -EINVAL;
                if (!(proposed.ss_flags & SS_DISABLE) && proposed.ss_size < 5120) return -ENOMEM;
                task.altstack = proposed;
            }
            return 0;
        }
        case 136: {
            if (a[1] != 8) return -EINVAL;
            uint64_t pending = task.pending & s.signal_mask;
            return s.memory().Write(a[0], &pending, 8) ? 0 : -EFAULT;
        }
        case 133: {
            long rc = temporary_mask(a[0], a[1]);
            return rc < 0 ? rc : kBlocked;
        }
        case 139: {
            SignalFrame frame{};
            StubContext context = task.context;
            if (!s.memory().Read(context.machine.sp, &frame, sizeof(frame))) {
                FinishGroup(task, SIGSEGV, SIGSEGV); return 0;
            }
            context.machine = frame.uc.uc_mcontext;
            if (!GuestWindow::Contains(context.machine.pc) ||
                !GuestWindow::Contains(context.machine.sp - 1) || (context.machine.sp & 15)) {
                FinishGroup(task, SIGSEGV, SIGSEGV); return 0;
            }
            memcpy(&s.signal_mask, &frame.uc.uc_sigmask, 8);
            s.signal_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
            task.altstack = frame.uc.uc_stack;
            task.context = context;
            return static_cast<long>(context.machine.regs[0]);
        }
        case 154: { // setpgid
            int pid = a[0] ? a[0] : s.pid, group = a[1] ? a[1] : pid;
            Task* target = Find(pid);
            if (!target || target->dead || (pid != s.pid && target->sentry->ppid != s.pid)) return -ESRCH;
            if (group < 0) return -EINVAL;
            if (target->sentry->session != s.session) return -EPERM;
            if (group != pid) {
                bool exists = false;
                for (const auto& t : tasks_) exists |= !t->dead && t->sentry->group == group && t->sentry->session == s.session;
                if (!exists) return -EPERM;
            }
            target->sentry->group = group; return 0;
        }
        case 155: case 156: {
            Task* target = a[0] ? Find(a[0]) : &task;
            if (!target || target->dead) return -ESRCH;
            return req.nr == 155 ? target->sentry->group : target->sentry->session;
        }
        case 157:
            if (s.group == s.pid) return -EPERM;
            s.group = s.session = s.pid; s.files().session = s.session; return s.pid;
        case 165: {
            if (static_cast<long>(a[0]) != RUSAGE_SELF && static_cast<long>(a[0]) != RUSAGE_CHILDREN) return -EINVAL;
            rusage usage{}; return s.memory().Write(a[1], &usage, sizeof(usage)) ? 0 : -EFAULT;
        }
        case 101: case 115: { // nanosleep / clock_nanosleep
            uintptr_t pointer = req.nr == 101 ? a[0] : a[2];
            timespec value{};
            if (!s.memory().Read(pointer, &value, sizeof(value))) return -EFAULT;
            if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000 ||
                static_cast<uint64_t>(value.tv_sec) > UINT64_MAX / 1000000000) return -EINVAL;
            if (req.nr == 115 && ((a[0] != CLOCK_MONOTONIC && a[0] != CLOCK_REALTIME) || (a[1] & ~TIMER_ABSTIME))) return -EINVAL;
            if (!task.deadline) {
                uint64_t duration = uint64_t(value.tv_sec) * 1000000000 + value.tv_nsec;
                if (req.nr == 115 && (a[1] & TIMER_ABSTIME)) {
                    timespec current{}; clock_gettime(a[0], &current);
                    uint64_t epoch = uint64_t(current.tv_sec) * 1000000000 + current.tv_nsec;
                    duration = duration > epoch ? duration - epoch : 0;
                }
                if (duration > UINT64_MAX - Now()) return -EINVAL;
                task.deadline = Now() + duration;
            }
            return Now() >= task.deadline ? 0 : kBlocked;
        }
        case 73: { // ppoll
            if (a[1] > 256) return -EINVAL;
            long rc = temporary_mask(a[3], a[4]); if (rc < 0) return rc;
            std::vector<pollfd> guest(a[1]), host(a[1]);
            if (!s.memory().Read(a[0], guest.data(), guest.size() * sizeof(pollfd))) return -EFAULT;
            host = guest;
            for (auto& fd : host) {
                if (fd.fd >= 0) fd.fd = s.files().HostFd(fd.fd);
                fd.revents = 0;
            }
            int ready = poll(host.data(), host.size(), 0);
            if (ready < 0) return -errno;
            ready = 0;
            for (size_t i = 0; i < guest.size(); ++i) {
                guest[i].revents = host[i].revents;
                if (guest[i].fd >= 0 && host[i].fd < 0) {
                    guest[i].revents = s.files().VirtualEvents(guest[i].fd, guest[i].events);
                }
                ready += guest[i].revents != 0;
            }
            if (a[2] && !task.deadline) {
                timespec timeout{};
                if (!s.memory().Read(a[2], &timeout, sizeof(timeout))) return -EFAULT;
                if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000 || timeout.tv_sec > 86400) return -EINVAL;
                task.deadline = Now() + uint64_t(timeout.tv_sec) * 1000000000 + timeout.tv_nsec;
            }
            if (!ready && (!task.deadline || Now() < task.deadline)) return kBlocked;
            return s.memory().Write(a[0], guest.data(), guest.size() * sizeof(pollfd)) ? ready : -EFAULT;
        }
        default: return {};
    }
}


void Kernel::LaunchTerminal(const std::shared_ptr<TerminalSession>& session, Sentry& init) {
    auto fail = [&](const std::string& error) { session->control->Append("\r\n" + error + "\r\n"); session->state = TerminalSession::kFailed; };
    if (session->control->stop) { session->state = TerminalSession::kExited; return; }
    size_t live = 0; for (const auto& t : tasks_) live += !t->reaped;
    if (live >= init.limits->tasks || next_pid_ == INT_MAX) { fail("Linux task limit reached."); return; }
    auto task = std::make_unique<Task>(); task->owned = std::make_unique<Sentry>(Sentry::LogFn{}); task->sentry = task->owned.get();
    auto& s = *task->sentry; s.files().ShareNamespace(init.files()); s.limits = init.limits; s.shared_memory = init.shared_memory;
    s.pid = s.tgid = s.group = s.session = next_pid_++; s.ppid = 1;
    s.files().lock_owner = s.files().current_tid = s.pid;
    s.files().session = s.session; s.files().foreground_group = s.group;
    auto binding = std::make_shared<TerminalBinding>(); binding->session = session; binding->pid = s.pid; binding->creating = session->create_user;
    std::string error;
    if (binding->creating) {
        s.arguments = {"/usr/sbin/useradd","--create-home","--user-group","--shell","/bin/bash","-K","HOME_MODE=0700","--",session->user};
        s.files().executable = s.arguments[0];
    } else if (!ConfigureSession(s,session->user,&error)) { fail(error); return; }
    CredentialScope scope(s.credentials);
    if (!binding->creating) {
        if (!s.files().UseTerminal("",&error)) { fail(error); return; }
        binding->master = s.files().terminal_transport; binding->tty = s.files().controlling_terminal();
        // Only the binding owns the master. Forked Linux children must not
        // accidentally keep their own terminal alive after it is closed.
        s.files().terminal_transport.reset();
    }
    binding->output = s.files().output;
    uint64_t used = MemoryUsed(); LoadedImage image;
    if (used >= s.limits->memory_bytes || !ResetScratch() ||
        !LoadProgram(*s.files().vfs,s.arguments[0],&image,&error,s.limits->memory_bytes-used)) {
        fail(error.empty() ? "Linux memory limit reached." : error); return;
    }
    task->stub = std::make_shared<StubProcess>();
    if (!task->stub->Start(image,&s,false,nullptr,&error)) { fail(error); return; }
    task->on_exit = [binding](int status) { binding->status = status; };
    Connect(*task); tasks_.push_back(std::move(task)); terminals_.push_back(binding);
    session->pid = binding->pid;
    if (!binding->creating) session->state = TerminalSession::kRunning;
}
void Kernel::PumpTerminals(Sentry& init) {
    for (const auto& session : runtime_->TakePending()) LaunchTerminal(session,init);
    std::vector<std::shared_ptr<TerminalSession>> created;
    for (const auto& binding : terminals_) {
        auto& b = *binding; auto& control = *b.session->control;
        auto signal_group = [&](int signal, bool closing = false) {
            if (!b.tty) return;
            for (auto& task : tasks_) if (!task->dead && task->sentry->session == b.tty->session &&
                (task->sentry->group == b.tty->foreground_group || (closing && task->sentry->tgid == b.pid))) {
                siginfo_t info{}; info.si_signo = signal; info.si_code = SI_KERNEL; Queue(*task,signal,info);
            }
        };
        if (b.master) {
            termios settings{}; bool isig = tcgetattr(b.master->fd,&settings) == 0 && (settings.c_lflag & ISIG);
            control.foreground_group = b.tty->foreground_group; control.signal_keys = isig;
            std::string additional = control.TakeInput();
            if (b.input.size() + additional.size() <= 65536) b.input += additional;
            unsigned rows, columns;
            if (control.TakeResize(&rows,&columns)) {
                winsize size{static_cast<unsigned short>(rows),static_cast<unsigned short>(columns),0,0};
                if (ioctl(b.master->fd,TIOCSWINSZ,&size) == 0) signal_group(SIGWINCH);
            }
            if (!b.input.empty()) {
                ssize_t n = write(b.master->fd,b.input.data(),b.input.size());
                if (n > 0) {
                    if (isig) for (ssize_t i = 0; i < n; ++i) {
                        unsigned char c = b.input[i];
                        if (c && c == settings.c_cc[VINTR]) signal_group(SIGINT);
                        else if (c && c == settings.c_cc[VQUIT]) signal_group(SIGQUIT);
                        else if (c && c == settings.c_cc[VSUSP]) signal_group(SIGTSTP);
                    }
                    b.input.erase(0,n);
                }
            }
            char bytes[4096];
            for (size_t total = 0; total < (256u << 10);) {
                ssize_t n = read(b.master->fd,bytes,sizeof(bytes)); if (n <= 0) break;
                control.Append(std::string(bytes,n)); total += n;
            }
        }
        if (!b.output->out.empty()) { control.Append(b.output->out); b.output->out.clear(); }
        if (!b.output->err.empty()) { control.Append(b.output->err); b.output->err.clear(); }
        if (b.creating && control.stop && b.status < 0) {
            if (Task* task = Find(b.pid)) FinishGroup(*task,SIGKILL,SIGKILL);
        }
        if (b.creating && b.status >= 0) {
            if (!b.status && !control.stop) { b.session->create_user = false; created.push_back(b.session); }
            else { control.Append("\r\nAccount creation failed (" + std::to_string(b.status) + ").\r\n"); b.session->state = TerminalSession::kFailed; }
            continue;
        }
        if (!b.creating && !b.closed && (control.stop || b.status >= 0)) {
            signal_group(SIGHUP,true); signal_group(SIGCONT,true);
            b.master.reset(); b.closed = true; b.closed_at = Now();
        }
        if (b.closed && b.status < 0 && Now() - b.closed_at > 500000000) {
            if (Task* task = Find(b.pid)) FinishGroup(*task,SIGKILL,SIGKILL);
        }
        if (!b.creating && b.status >= 0) { b.session->exit_status = b.status; b.session->state = TerminalSession::kExited; }
    }
    terminals_.erase(std::remove_if(terminals_.begin(),terminals_.end(),[](const auto& b) { return b->status >= 0; }),terminals_.end());
    for (const auto& session : created) LaunchTerminal(session,init);
}

RunResult Kernel::Run(const LoadedImage& image, Sentry* sentry, bool test_gate) {
    start_ = Now(); runtime_ = sentry->runtime;
    if (!sentry->limits) sentry->limits = std::make_shared<RunLimits>();
    int quota = sentry->files().vfs->SetDiskLimit(sentry->limits->disk_bytes);
    if (quota < 0) { result_.error = std::string("rootfs quota: ") + strerror(-quota); return result_; }
    sentry->shared_memory = std::make_shared<SharedMemory>();
    if (!sentry->shared_memory->Create(&result_.error)) return result_;
    // A broker write to a closed pipe must become guest EPIPE/SIGPIPE.
    sigset_t block; sigemptyset(&block); sigaddset(&block, SIGPIPE);
    sigset_t previous; pthread_sigmask(SIG_BLOCK, &block, &previous);
    auto root = std::make_unique<Task>(); root->sentry = sentry;
    root->audited = !test_gate;
    if (runtime_) { root->awaiting = false; sentry->arguments = {"init"}; sentry->files().executable = "/sbin/init"; }
    else {
        root->stub = std::make_shared<StubProcess>();
        if (!root->stub->Start(image, sentry, test_gate, nullptr, &result_.error)) {
            pthread_sigmask(SIG_SETMASK, &previous, nullptr); return result_;
        }
    }
    Connect(*root); tasks_.push_back(std::move(root));
    result_.entered = true;
    auto transport = sentry->files().terminal_transport;
    std::string input = sentry->files().terminal_input;
    size_t input_sent = 0, published_out = 0, published_err = 0;
    auto terminal_state = sentry->files().controlling_terminal();
    auto signal_group = [&](int signal) {
        if (!terminal_state) return;
        for (auto& task : tasks_) if (!task->dead && task->sentry->group == terminal_state->foreground_group) {
            siginfo_t info{}; info.si_signo = signal; info.si_code = SI_KERNEL;
            Queue(*task, signal, info);
        }
    };
    auto terminal = [&] {
        termios settings{};
        bool isig = transport && tcgetattr(transport->fd, &settings) == 0 && (settings.c_lflag & ISIG);
        if (sentry->control) {
            if (terminal_state) sentry->control->foreground_group = terminal_state->foreground_group;
            sentry->control->signal_keys = isig;
            std::string additional = sentry->control->TakeInput();
            if (input_sent) { input.erase(0, input_sent); input_sent = 0; }
            if (input.size() + additional.size() <= 65536) input += additional;
            unsigned rows, columns;
            if (transport && sentry->control->TakeResize(&rows, &columns)) {
                winsize size{static_cast<unsigned short>(rows), static_cast<unsigned short>(columns), 0, 0};
                if (ioctl(transport->fd, TIOCSWINSZ, &size) == 0) signal_group(SIGWINCH);
            }
        }
        if (transport) {
            if (input_sent < input.size()) {
                // Host PTYs cannot signal virtual process groups. Let their
                // line discipline echo/flush control bytes, then route signals
                // ourselves to the guest's foreground process group.
                ssize_t n = write(transport->fd, input.data() + input_sent, input.size() - input_sent);
                if (n > 0) {
                    if (isig) for (ssize_t i = 0; i < n; ++i) {
                        unsigned char c = input[input_sent + i];
                        if (c && c == settings.c_cc[VINTR]) signal_group(SIGINT);
                        else if (c && c == settings.c_cc[VQUIT]) signal_group(SIGQUIT);
                        else if (c && c == settings.c_cc[VSUSP]) signal_group(SIGTSTP);
                    }
                    input_sent += n;
                }
            }
            char buffer[4096];
            for (;;) {
                ssize_t n = read(transport->fd, buffer, sizeof(buffer));
                if (n <= 0) break;
                if (sentry->files().output->out.size() + sentry->files().output->err.size() + n > sentry->limits->output_bytes) {
                    result_.error = "terminal output limit exceeded"; break;
                }
                sentry->files().output->out.append(buffer, n);
            }
        }
        if (sentry->control) {
            const auto& output = *sentry->files().output;
            if (output.out.size() > published_out) { sentry->control->Append(output.out.substr(published_out)); published_out = output.out.size(); }
            if (output.err.size() > published_err) { sentry->control->Append(output.err.substr(published_err)); published_err = output.err.size(); }
        }
    };
    unsigned idle = 0;
    uint64_t last_sample=0;
    while (GroupAlive(1) && result_.error.empty()) {
        uint64_t sample_time=Now();
        if (sample_time-last_sample>=50000000) {
            result_.broker_rss_sampled_peak_bytes=std::max(result_.broker_rss_sampled_peak_bytes,BrokerResidentBytes());
            last_sample=sample_time;
        }
        terminal();
        if (runtime_) { PumpTerminals(*sentry); if (runtime_->stop) break; }
        if (sentry->limits->wall_time_ms && (Now() - start_) / 1000000 > sentry->limits->wall_time_ms) { result_.error = "guest exceeded its wall-time limit"; break; }
        if (sentry->control && sentry->control->stop) { result_.error = "session stopped"; break; }
        bool progress = false;
        size_t count = tasks_.size();
        for (size_t i = 0; i < count && result_.error.empty(); ++i) {
            Task& task = *tasks_[i];
            if (task.dead || task.stopped || task.vfork_wait || !task.stub) continue;
            if (task.awaiting && !task.applying && !task.blocked && !task.notified && HasSignal(task) && task.stub->channel.boot_stage() >= 5) {
                kill(task.stub->pid, SIGUSR1); task.notified = true;
            }
            bool needs_slice = false;
            for (const auto& peer : tasks_) needs_slice |= peer.get() != &task && !peer->dead && peer->ready && peer->stub == task.stub;
            if (task.awaiting && !task.applying && !task.notified && needs_slice &&
                Now() - task.last_slice > 2000000 && task.stub->channel.boot_stage() >= 5) {
                kill(task.stub->pid, SIGUSR1); task.notified = true;
            }
            if (task.ready) continue;
            if (task.blocked) {
                if (task.deferred) {
                    long value = *task.deferred; task.deferred.reset(); Reply(task, value); progress = true; continue;
                }
                AddressSpace::StubOp op;
                long value = task.sentry->Handle(task.request, &op);
                if (value == kBlocked && HasSignal(task)) {
                    value = -EINTR;
                    if (task.request.nr == 101 || (task.request.nr == 115 && !(task.request.args[1] & TIMER_ABSTIME))) {
                        uint64_t remaining = task.deadline > Now() ? task.deadline - Now() : 0;
                        timespec rest{static_cast<time_t>(remaining / 1000000000), static_cast<long>(remaining % 1000000000)};
                        uintptr_t destination = task.request.nr == 101 ? task.request.args[1] : task.request.args[3];
                        if (destination && !task.sentry->memory().Write(destination, &rest, sizeof(rest))) value = -EFAULT;
                    }
                }
                if (value != kBlocked) { Reply(task, value); progress = true; }
                continue;
            }
            if (!task.awaiting) continue;
            SyscallRequest req;
            const auto event = count == 1 ? task.stub->channel.Wait(task.stub->pid, &req)
                                         : task.stub->channel.Poll(task.stub->pid, &req);
            if (event == SyscallChannel::Event::kTimeout) continue;
            progress = true; task.awaiting = false;
            if (!task.audited) {
                if (!task.stub->Audit(&result_.error)) break;
                task.audited = true;
            }
            switch (event) {
                case SyscallChannel::Event::kRequest: {
                    if (!result_.syscalls) result_.startup_ns=Now()-start_;
                    if (req.nr == -1) task.notified = false;
                    task.request = req;
                    task.context = task.stub->channel.context();
                    AddressSpace::StubOp op;
                    const uint64_t before = Now();
                    long value = task.sentry->Handle(req, &op);
                    handler_ns_ += Now() - before;
                    ++result_.syscalls;
                    if (task.sentry != sentry && value != kBlocked) sentry->RecordExternal(req, value == kReplaced ? 0 : value);
                    if (task.dead || value == kReplaced) break;
                    if (task.sentry->exited()) { Finish(task, task.sentry->exit_status() << 8); break; }
                    if (value == kBlocked) task.blocked = true;
                    else if (op.kind != AddressSpace::StubOp::kNone) {
                        task.awaiting = true; task.applying = true; task.stub->channel.Reply(value, op);
                    }
                    else Reply(task, value);
                    break;
                }
                case SyscallChannel::Event::kApplied: {
                    task.applying = false;
                    AddressSpace::StubOp next;
                    long value = task.sentry->CompleteMemoryOp(task.stub->channel.applied_result(), &next);
                    if (next.kind != AddressSpace::StubOp::kNone) {
                        task.awaiting = true; task.applying = true; task.stub->channel.Reply(value, next);
                    }
                    else Reply(task, value);
                    break;
                }
                case SyscallChannel::Event::kExited:
                    if (!task.sentry->exited()) result_.error = "unsolicited stub exit message";
                    else Finish(task, task.sentry->exit_status() << 8);
                    break;
                case SyscallChannel::Event::kFaulted: {
                    int sig = task.stub->channel.fault_signal();
                    if (sig < 0 && sig >= -4095) result_.error = std::string("stub setup failed: ") + strerror(-sig);
                    else if (sig <= 0 || sig > 64) result_.error = "invalid stub fault message";
                    else Finish(task, sig, sig);
                    break;
                }
                case SyscallChannel::Event::kPeerLost: {
                    task.stub->Stop();
                    int status = task.stub->host_status;
                    int sig = WIFSIGNALED(status) ? WTERMSIG(status) : SIGKILL;
                    auto lost = task.stub;
                    for (auto& other : tasks_) if (!other->dead && other->stub == lost) FinishGroup(*other, sig, sig);
                    break;
                }
                case SyscallChannel::Event::kTimeout: break;
            }
        }
        Schedule();
        sentry->files().unix_namespace->Collect();
        if (progress) idle = 0;
        // Sleep longer when every guest is waiting. A guest executing between
        // syscall messages needs a short polling grace period, not a 2 ms
        // penalty for each syscall (which also delays terminal responses).
        else if (runtime_ && std::none_of(tasks_.begin(), tasks_.end(), [](const auto& task) {
            return task->stub && !task->dead && !task->stopped && !task->vfork_wait && !task->blocked;
        })) { timespec nap{0, tasks_.size() == 1 ? 20000000 : 2000000}; nanosleep(&nap,nullptr); }
        else if (++idle > 100) { timespec nap{0, 100000}; nanosleep(&nap, nullptr); idle = 0; }
        tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(),
            [](const auto& task) { return task->reaped; }), tasks_.end());
    }
    const uint64_t elapsed = Now() - start_;
    if (runtime_) for (const auto& session : runtime_->Sessions()) {
        session->exit_status = 137; if (session->state < TerminalSession::kExited) session->state = TerminalSession::kExited;
    }
    terminal();
    for (auto& task : tasks_) {
        if (task->stub) task->stub->Stop(); task->sentry->dispatch = {}; task->sentry->allow_memory = {};
    }
    // Consume any SIGPIPE generated by broker pipe writes before restoring
    // the launcher's mask, so it cannot terminate the Android process later.
    timespec zero{}; while (sigtimedwait(&block, nullptr, &zero) >= 0) {}
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    if (result_.syscalls) {
        result_.ns_per_syscall = double(elapsed) / result_.syscalls;
        result_.ns_in_handler = double(handler_ns_) / result_.syscalls;
    }
    return result_;
}
}  // namespace
RunResult RunGuest(const LoadedImage& image, Sentry* sentry, bool test_gate) {
    Kernel kernel;
    return kernel.Run(image, sentry, test_gate);
}
}  // namespace goblin
