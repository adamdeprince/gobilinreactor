#include "guest_memory.h"

#include <cstring>

#include <sys/uio.h>
#include <unistd.h>

#include "guest_layout.h"

namespace goblin {

bool GuestMemory::ReadString(uintptr_t addr, std::string* out, size_t limit) const {
    out->clear();
    for (size_t i = 0; i < limit; ++i) {
        char c;
        if (!Read(addr + i, &c, 1)) return false;
        if (!c) return true;
        out->push_back(c);
    }
    return false;
}

bool GuestMemory::InWindow(uintptr_t addr, size_t len) {
    if (len == 0) return true;
    return GuestWindow::ContainsRange(addr, len);
}

bool GuestMemory::Read(uintptr_t addr, void* dst, size_t len) const {
    if (!InWindow(addr, len)) return false;
    if (len == 0) return true;
    if (pid_ == 0) {
        memcpy(dst, reinterpret_cast<const void*>(GuestWindow::BrokerAddress(addr)), len);
        return true;
    }
    iovec local{dst, len};
    iovec remote{reinterpret_cast<void*>(addr), len};
    return process_vm_readv(pid_, &local, 1, &remote, 1, 0) ==
           static_cast<ssize_t>(len);
}

bool GuestMemory::Write(uintptr_t addr, const void* src, size_t len) const {
    if (!InWindow(addr, len)) return false;
    if (len == 0) return true;
    if (pid_ == 0) {
        memcpy(reinterpret_cast<void*>(GuestWindow::BrokerAddress(addr)), src, len);
        return true;
    }
    iovec local{const_cast<void*>(src), len};
    iovec remote{reinterpret_cast<void*>(addr), len};
    return process_vm_writev(pid_, &local, 1, &remote, 1, 0) ==
           static_cast<ssize_t>(len);
}

}  // namespace goblin
