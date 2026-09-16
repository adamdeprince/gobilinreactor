#include "guest_layout.h"

#include <cerrno>
#include <cstring>

#include <sys/mman.h>

namespace goblin {

uintptr_t GuestWindow::start_ = 0;

bool GuestWindow::Reserve(std::string* err) {
    if (start_ != 0) return true;

    // Over-reserve by the alignment so there is guaranteed to be an aligned
    // window inside, then give back the slack at both ends. Asking the kernel
    // for a specific address fails in an app process, where ART has already
    // taken much of the low address space.
    const size_t raw_size = kSize + kAlignment;
    void* raw = mmap(nullptr, raw_size, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) {
        if (err) *err = std::string("reserving guest window: ") + strerror(errno);
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(raw);
    const uintptr_t aligned = (base + kAlignment - 1) & ~(kAlignment - 1);

    if (aligned > base) munmap(raw, aligned - base);
    const uintptr_t window_end = aligned + kSize;
    const uintptr_t raw_end = base + raw_size;
    if (raw_end > window_end) {
        munmap(reinterpret_cast<void*>(window_end), raw_end - window_end);
    }

    start_ = aligned;
    return true;
}

}  // namespace goblin
