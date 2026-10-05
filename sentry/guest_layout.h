// Where a guest address space lives inside the host's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace goblin {

// A high window holds PIEs, libraries, stacks and mmap allocations. A separate
// low window accepts fixed-address Debian ELFs (Python and GCC's cc1). The broker
// stages low pages at an unrelated address; only the isolated child moves them
// into place, after removing its inherited Android mappings.
class GuestWindow {
public:
    static constexpr size_t kSize = 1ull << 30;       // 1 GiB
    static constexpr size_t kAlignment = 1ull << 32;  // 4 GiB
    static constexpr size_t kStackSize = 8u << 20;    // 8 MiB
    static constexpr uintptr_t kLowStart = 0x10000;
    static constexpr uintptr_t kLowEnd = 0x4000000;   // 64 MiB in the child only
    static_assert(kSize <= (1ull << 30), "stub filter arithmetic requires a window of at most 1 GiB");

    // Idempotent. Safe to call more than once; only the first call reserves.
    static bool Reserve(std::string* err);
    static bool Reset(); // Reset broker scratch mappings; live stubs are separate.

    static uintptr_t start() { return start_; }
    static uintptr_t end() { return start_ + kSize; }
    static uint32_t high_word() { return static_cast<uint32_t>(start_ >> 32); }
    static uintptr_t low_shadow() { return low_shadow_; }
    // Call only after validating the complete guest range. This is for broker
    // staging, never for process_vm_* addresses or values sent to the guest.
    static uintptr_t BrokerAddress(uintptr_t addr) {
        return addr >= kLowStart && addr < kLowEnd ? low_shadow_ + addr - kLowStart : addr;
    }

    static bool Contains(uintptr_t addr) {
        return ContainsRange(addr, 1);
    }
    static bool ContainsRange(uintptr_t addr, size_t len) {
        if (!start_) return false;
        return (addr >= start_ && addr < end() && len <= end() - addr) ||
               (addr >= kLowStart && addr < kLowEnd && len <= kLowEnd - addr);
    }

    // Stack at the top of the window, growing down.
    static uintptr_t stack_top() { return end(); }
    static uintptr_t stack_bottom() { return end() - kStackSize; }

    // PIE images are biased to here. ET_EXEC images must already fall inside.
    static uintptr_t pie_base() { return start_ + 0x1000000; }  // +16 MiB
    static uintptr_t signal_trampoline() { return start_ + 0x100000; }

private:
    static uintptr_t start_;
    static uintptr_t low_shadow_;
};

}  // namespace goblin
