#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <linux/filter.h>

namespace goblin {
// The syscall gate is public to the guest. Its arguments must be safe even
// when chosen by hostile code, rather than by our signal handler.
std::vector<sock_filter> BuildStubFilter(uintptr_t gate_pc, uintptr_t guest_start,
                                        size_t guest_size, uintptr_t state_addr,
                                        int shared_fd = -1, int transfer_fd = -1);
}
