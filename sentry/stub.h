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
    int fault_signal = 0;
    uint64_t syscalls = 0;
    uint64_t wall_time_ns = 0;
    uint64_t startup_ns = 0; // mount/load/fork until the first guest trap
    uint64_t broker_rss_start_bytes = 0;
    uint64_t broker_rss_end_bytes = 0;
    uint64_t broker_rss_sampled_peak_bytes = 0; // whole app process, 50 ms samples
    double ns_per_syscall = 0;  // wall clock, guest entry to exit
    double ns_in_handler = 0;   // of which, spent inside the SIGSYS handler
    std::string error;      // setup failure, or how the guest died
};

// Forks a stub process, runs `img` in it, and services its syscalls until it
// exits. Returns when the guest is gone.
//
// Only a freestanding runtime remains in the child. It does not depend on
// bionic TLS, and its syscall gate remains constrained if called by the guest.
RunResult RunGuest(const LoadedImage& img, Sentry* sentry, bool expose_test_gate = false);

}  // namespace goblin
