// Where a guest address space lives inside the host's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace goblin {

// The guest is confined to one contiguous window, reserved once at start-up.
//
// The kernel picks where, because an app process already has ART's heap and dex
// mappings scattered through the low address space and a fixed address collides
// with them. What is fixed is the *alignment*: the window is aligned to 4 GiB and
// no larger than 4 GiB, so every address in it shares one high word. That is what
// lets the seccomp filter separate guest syscalls from the sentry's own with a
// single comparison on the high half of the instruction pointer.
class GuestWindow {
public:
    static constexpr size_t kSize = 1ull << 30;       // 1 GiB
    static constexpr size_t kAlignment = 1ull << 32;  // 4 GiB
    static constexpr size_t kStackSize = 8u << 20;    // 8 MiB
    static_assert(kSize <= kAlignment, "window must not straddle a 4 GiB boundary");

    // Idempotent. Safe to call more than once; only the first call reserves.
    static bool Reserve(std::string* err);

    static uintptr_t start() { return start_; }
    static uintptr_t end() { return start_ + kSize; }
    static uint32_t high_word() { return static_cast<uint32_t>(start_ >> 32); }

    static bool Contains(uintptr_t addr) {
        return start_ != 0 && addr >= start_ && addr < start_ + kSize;
    }

    // Stack at the top of the window, growing down.
    static uintptr_t stack_top() { return end(); }
    static uintptr_t stack_bottom() { return end() - kStackSize; }

    // PIE images are biased to here. ET_EXEC images must already fall inside.
    static uintptr_t pie_base() { return start_ + 0x1000000; }  // +16 MiB

private:
    static uintptr_t start_;
};

}  // namespace goblin
