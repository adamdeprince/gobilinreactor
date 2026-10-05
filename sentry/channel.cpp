#include "channel.h"
#include <cerrno>
#include <cstring>
#include <linux/futex.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <unistd.h>

namespace goblin {
namespace {
constexpr uint32_t Bit(uint32_t state) { return state < 32 ? 1u << state : 0; }
constexpr int kSpinIterations = 20000;
}

size_t SyscallChannel::mapping_size() const {
    const size_t page = sysconf(_SC_PAGESIZE);
    return (sizeof(StubShared) + page - 1) & ~(page - 1);
}

SyscallChannel::~SyscallChannel() {
    if (shared_ != nullptr) munmap(shared_, mapping_size());
    if (fd_ >= 0) close(fd_);
    for (int fd : transfer_) if (fd >= 0) close(fd);
}

bool SyscallChannel::Create(std::string* err) {
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, transfer_) < 0) {
        *err = std::string("creating capability channel: ") + strerror(errno); return false;
    }
    fd_ = static_cast<int>(syscall(__NR_memfd_create, "goblin-channel", MFD_CLOEXEC));
    if (fd_ < 0 || ftruncate(fd_, mapping_size()) != 0) {
        *err = std::string("creating channel: ") + strerror(errno);
        return false;
    }
    void* p = mmap(nullptr, mapping_size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) {
        *err = std::string("mapping channel: ") + strerror(errno);
        return false;
    }
    shared_ = static_cast<StubShared*>(p);
    close(fd_);
    fd_ = -1;
    memset(shared_, 0, sizeof(*shared_));
    return true;
}

void SyscallChannel::ParentAfterFork() {
    close(transfer_[1]); transfer_[1] = -1;
}

uint32_t SyscallChannel::Await(uint32_t mask, pid_t watch) {
    for (int i = 0; i < kSpinIterations; ++i) {
        const uint32_t s = __atomic_load_n(&shared_->state, __ATOMIC_ACQUIRE);
        if ((mask & Bit(s)) != 0) return s;
        if (s > STUB_APPLIED) return kNoState;
        __asm__ volatile("yield" ::: "memory");
    }
    constexpr timespec nap{0, 50 * 1000 * 1000};
    __atomic_fetch_add(&shared_->sleepers, 1, __ATOMIC_SEQ_CST);
    uint32_t s = __atomic_load_n(&shared_->state, __ATOMIC_SEQ_CST);
    if ((mask & Bit(s)) == 0) syscall(__NR_futex, &shared_->state, FUTEX_WAIT, s, &nap, nullptr, 0);
    __atomic_fetch_sub(&shared_->sleepers, 1, __ATOMIC_SEQ_CST);
    s = __atomic_load_n(&shared_->state, __ATOMIC_ACQUIRE);
    if ((mask & Bit(s)) != 0) return s;
    siginfo_t info{};
    const int result = waitid(P_PID, watch, &info, WEXITED | WNOHANG | WNOWAIT);
    if ((result == 0 && info.si_pid == watch) || (result < 0 && errno == ECHILD)) {
        s = __atomic_load_n(&shared_->state, __ATOMIC_ACQUIRE);
        return (mask & Bit(s)) ? s : kNoState;
    }
    return STUB_IDLE;  // let the owner enforce its wall-clock deadline
}

void SyscallChannel::Post(uint32_t state) {
    __atomic_store_n(&shared_->state, state, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&shared_->sleepers, __ATOMIC_SEQ_CST)) {
        syscall(__NR_futex, &shared_->state, FUTEX_WAKE, 1, nullptr, nullptr, 0);
    }
}

SyscallChannel::Event SyscallChannel::Wait(pid_t child, SyscallRequest* req) {
    const uint32_t state = Await(Bit(STUB_REQUEST) | Bit(STUB_APPLIED) |
                                 Bit(STUB_EXITED) | Bit(STUB_FAULTED), child);
    return Decode(state, req);
}

SyscallChannel::Event SyscallChannel::Poll(pid_t child, SyscallRequest* req) {
    uint32_t state = __atomic_load_n(&shared_->state, __ATOMIC_ACQUIRE);
    if (state == STUB_REPLY || state == STUB_IDLE) {
        siginfo_t info{};
        int rc = waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT);
        if ((rc == 0 && info.si_pid == child) || (rc < 0 && errno == ECHILD)) {
            state = __atomic_load_n(&shared_->state, __ATOMIC_ACQUIRE);
            if (state == STUB_REPLY || state == STUB_IDLE) return Event::kPeerLost;
        } else return Event::kTimeout;
    }
    return Decode(state, req);
}

SyscallChannel::Event SyscallChannel::Decode(uint32_t state, SyscallRequest* req) {
    switch (state) {
        case STUB_REQUEST:
            req->nr = shared_->nr;
            for (int i = 0; i < 6; ++i) req->args[i] = shared_->args[i];
            req->pc = shared_->pc;
            return Event::kRequest;
        case STUB_APPLIED: return Event::kApplied;
        case STUB_EXITED: return Event::kExited;
        case STUB_FAULTED: return Event::kFaulted;
        case STUB_IDLE: return Event::kTimeout;
        default: return Event::kPeerLost;
    }
}

void SyscallChannel::Reply(long ret, const AddressSpace::StubOp& op) {
    shared_->ret = ret;
    shared_->op_kind = static_cast<int32_t>(op.kind);
    shared_->op_addr = op.addr;
    shared_->op_len = op.len;
    shared_->op_prot = op.prot;
    shared_->op_flags = op.flags;
    shared_->op_fd = op.fd;
    shared_->op_offset = op.offset;
    shared_->op_new_addr = op.new_addr;
    shared_->op_new_len = op.new_len;
    if (op.kind == AddressSpace::StubOp::kMapFile) {
        char byte = 0;
        iovec iov{&byte, 1};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
        msghdr message{};
        message.msg_iov = &iov; message.msg_iovlen = 1;
        message.msg_control = control; message.msg_controllen = sizeof(control);
        auto* cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &op.fd, sizeof(int));
        ssize_t n;
        do { n = sendmsg(transfer_[0], &message, MSG_NOSIGNAL | MSG_DONTWAIT); } while (n < 0 && errno == EINTR);
        if (n != 1) { shared_->op_kind = AddressSpace::StubOp::kError; shared_->op_fd = n < 0 ? -errno : -EIO; }
    }
    Post(STUB_REPLY);
}

int SyscallChannel::exit_status() const { return shared_->exit_status; }
int SyscallChannel::fault_signal() const { return shared_->fault_sig; }
int SyscallChannel::fault_code() const { return shared_->fault_code; }
uintptr_t SyscallChannel::fault_pc() const { return shared_->fault_pc; }
uintptr_t SyscallChannel::fault_addr() const { return shared_->fault_addr; }
long SyscallChannel::applied_result() const { return shared_->applied_result; }
int SyscallChannel::boot_stage() const { return shared_->boot_stage; }
StubContext SyscallChannel::context() const { return shared_->context; }
void SyscallChannel::SetContext(const StubContext& context) {
    shared_->context = context;
    shared_->context_changed = 1;
}
}
