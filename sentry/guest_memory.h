// Reading and writing another process's guest memory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <sys/types.h>

namespace goblin {

// The sentry and the guest no longer share an address space, so every access to
// guest memory crosses a process boundary. A pid of 0 means "this process" and
// degrades to memcpy, which is what the pre-fork setup path needs while it is
// still building the guest's initial stack.
class GuestMemory {
public:
    GuestMemory() = default;
    explicit GuestMemory(pid_t pid) : pid_(pid) {}

    void set_pid(pid_t pid) { pid_ = pid; }
    pid_t pid() const { return pid_; }

    bool Read(uintptr_t addr, void* dst, size_t len) const;
    bool Write(uintptr_t addr, const void* src, size_t len) const;
    bool ReadString(uintptr_t addr, std::string* out, size_t limit = 4096) const;

private:
    // Guards against a guest handing us a pointer outside its own window.
    static bool InWindow(uintptr_t addr, size_t len);

    pid_t pid_ = 0;
};

}  // namespace goblin
