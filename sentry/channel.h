// The syscall channel between a stub process and the sentry.
#pragma once

#include <cstdint>
#include <string>

#include "address_space.h"
#include "sentry.h"
#include "stub_abi.h"

namespace goblin {

// Shared wire storage, a state word, and futex. The stub posts a syscall and blocks;
// the sentry services it and posts the result back.
//
// Both sides spin briefly before sleeping. A syscall round trip is the hot path
// of the entire system, and futex(2) costs two syscalls per side -- which, on a
// design whose whole purpose is intercepting syscalls, is worth avoiding whenever
// the peer is about to answer anyway.
//
// There is one outstanding request per native address space. The broker
// time-slices guest task contexts on that stub and preserves blocked requests
// separately; guest threads do not get native host thread privileges.
class SyscallChannel {
public:
    enum class Event { kRequest, kApplied, kExited, kFaulted, kPeerLost, kTimeout };

    ~SyscallChannel();

    // Called before fork, so both processes inherit the mapping.
    bool Create(std::string* err);
    int transfer_fd() const { return transfer_[1]; }
    void ParentAfterFork();

    // --- sentry side ---
    // `child` is watched so a stub that dies without reporting cannot wedge the
    // sentry in a futex wait forever.
    Event Wait(pid_t child, SyscallRequest* req);
    Event Poll(pid_t child, SyscallRequest* req);
    void Reply(long ret, const AddressSpace::StubOp& op);

    int exit_status() const;
    int fault_signal() const;
    int fault_code() const;
    uintptr_t fault_pc() const;
    uintptr_t fault_addr() const;
    long applied_result() const;
    int boot_stage() const;
    uintptr_t address() const { return reinterpret_cast<uintptr_t>(shared_); }
    size_t mapping_size() const;
    StubContext context() const;
    void SetContext(const StubContext& context);

private:
    static constexpr uint32_t kNoState = 0xffffffffu;

    // Waits for the state word to enter one of `mask`'s states, spinning first.
    uint32_t Await(uint32_t mask, pid_t watch);
    void Post(uint32_t state);
    Event Decode(uint32_t state, SyscallRequest* req);

    StubShared* shared_ = nullptr;
    int fd_ = -1;
    int transfer_[2] = {-1, -1};
};

}  // namespace goblin
