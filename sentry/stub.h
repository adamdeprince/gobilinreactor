// Entering a guest and trapping everything it does.
#pragma once

#include <cstdint>
#include <string>

#include "elf_loader.h"
#include "sentry.h"

namespace goblin {

struct RunResult {
    bool entered = false;   // the guest actually started executing
    bool exited = false;    // it called exit/exit_group
    int status = 0;
    uint64_t syscalls = 0;
    double ns_per_syscall = 0;  // wall clock, guest entry to exit
    double ns_in_handler = 0;   // of which, spent inside the SIGSYS handler
    std::string error;      // setup failure, or how the guest died
};

// Runs `img` to completion on the calling thread.
//
// The calling thread acquires a seccomp filter, which is permanent and cannot be
// removed, so this must be given a thread of its own. The filter traps by
// instruction pointer rather than by syscall number: anything issued from inside
// the guest window is the guest's, anything else is the sentry's own and passes
// straight through. That is what lets the syscall handler do real work -- open
// files, log, allocate -- on the very thread it is servicing.
RunResult RunGuest(const LoadedImage& img, Sentry* sentry);

}  // namespace goblin
