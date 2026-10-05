#include "address_space.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>

#include <sys/mman.h>
#include <unistd.h>

#include "guest_layout.h"

namespace goblin {
namespace {

size_t PageSize() {
    static const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return ps;
}

uintptr_t AlignDown(uintptr_t v) { return v & ~static_cast<uintptr_t>(PageSize() - 1); }
uintptr_t AlignUp(uintptr_t v) { return AlignDown(v + PageSize() - 1); }

bool RoundLength(size_t len, size_t* rounded) {
    if (len > SIZE_MAX - (PageSize() - 1)) return false;
    *rounded = AlignUp(len);
    return true;
}

bool ValidProtection(int prot) {
    return (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) == 0;
}

bool InWindow(uintptr_t start, uintptr_t end) {
    return end > start && GuestWindow::ContainsRange(start, end - start);
}

}  // namespace

void AddressSpace::Init(uintptr_t image_end) {
    vmas_.clear();
    brk_start_ = brk_ = AlignUp(image_end);
    // mmap allocates downward from just below the stack, growing towards the
    // heap growing up. They meet in the middle and whoever gets there first
    // gets ENOMEM, which is the same bargain Linux makes.
    mmap_top_ = GuestWindow::stack_bottom();
}

void AddressSpace::Reserve(uintptr_t start, uintptr_t end, int prot, int flags) {
    if (end <= start) return;
    Insert(AlignDown(start), AlignUp(end), prot, flags);
}
const AddressSpace::Vma* AddressSpace::Lookup(uintptr_t address) const {
    for (const auto& v : vmas_) if (address >= v.start && address < v.end) return &v;
    return nullptr;
}
void AddressSpace::FileBacking(uintptr_t start, size_t size, std::shared_ptr<HostFile> file, uint64_t offset) {
    std::vector<Vma> pieces;
    for (const auto& v : vmas_) {
        if (v.end <= start || v.start >= start + size) { pieces.push_back(v); continue; }
        if (v.start < start) { Vma left = v; left.end = start; pieces.push_back(left); }
        Vma middle = v; middle.start = std::max(start, v.start); middle.end = std::min(start + size, v.end);
        middle.file = file; middle.file_offset = offset + middle.start - start;
        middle.flags &= ~MAP_ANONYMOUS; pieces.push_back(middle);
        if (v.end > start + size) {
            Vma right = v; right.start = start + size;
            if (right.shared_backed) right.backing_offset += right.start - v.start;
            if (right.file) right.file_offset += right.start - v.start;
            pieces.push_back(right);
        }
    }
    vmas_ = std::move(pieces); Coalesce();
}
long AddressSpace::Msync(uintptr_t addr, size_t len, int flags, StubOp* op) {
    *op = {};
    if (addr % PageSize() || (flags & ~(MS_SYNC | MS_ASYNC | MS_INVALIDATE)) ||
        ((flags & MS_SYNC) && (flags & MS_ASYNC))) return -EINVAL;
    if (!RoundLength(len, &len) || addr > UINTPTR_MAX - len) return -ENOMEM;
    if (!len) return 0;
    if (!InWindow(addr, addr + len) || !IsMapped(addr, addr + len)) return -ENOMEM;
    *op = {StubOp::kSync, addr, len, 0, flags}; return 0;
}
void AddressSpace::AfterFork() {
    vmas_.erase(std::remove_if(vmas_.begin(), vmas_.end(),
        [](const Vma& v) { return !v.forkable; }), vmas_.end());
}
void AddressSpace::SharedBacking(uintptr_t start, size_t size, uint64_t offset) {
    std::vector<Vma> pieces;
    for (const auto& v : vmas_) {
        if (v.end <= start || v.start >= start + size) { pieces.push_back(v); continue; }
        if (v.start < start) { Vma left = v; left.end = start; pieces.push_back(left); }
        Vma middle = v; middle.start = std::max(start, v.start); middle.end = std::min(start + size, v.end);
        middle.shared_backed = true; middle.backing_offset = offset + middle.start - start;
        pieces.push_back(middle);
        if (v.end > start + size) {
            Vma right = v; right.start = start + size;
            if (right.shared_backed) right.backing_offset += right.start - v.start;
            if (right.file) right.file_offset += right.start - v.start;
            pieces.push_back(right);
        }
    }
    vmas_ = std::move(pieces);
    Coalesce();
}

bool AddressSpace::IsFree(uintptr_t start, uintptr_t end) const {
    for (const Vma& v : vmas_) {
        if (v.start < end && start < v.end) return false;
    }
    return true;
}

bool AddressSpace::IsMapped(uintptr_t start, uintptr_t end) const {
    uintptr_t covered = start;
    for (const Vma& v : vmas_) {
        if (v.end <= covered) continue;
        if (v.start > covered) return false;  // hole
        covered = v.end;
        if (covered >= end) return true;
    }
    return covered >= end;
}

void AddressSpace::Remove(uintptr_t start, uintptr_t end) {
    std::vector<Vma> kept;
    kept.reserve(vmas_.size() + 1);
    for (const Vma& v : vmas_) {
        if (v.end <= start || v.start >= end) {
            kept.push_back(v);
            continue;
        }
        if (v.start < start) { Vma left = v; left.end = start; kept.push_back(left); }
        if (v.end > end) {
            Vma right = v; right.start = end;
            if (right.shared_backed) right.backing_offset += end - v.start;
            if (right.file) right.file_offset += end - v.start;
            kept.push_back(right);
        }
    }
    vmas_ = std::move(kept);
    std::sort(vmas_.begin(), vmas_.end(),
              [](const Vma& a, const Vma& b) { return a.start < b.start; });
}

void AddressSpace::Insert(uintptr_t start, uintptr_t end, int prot, int flags) {
    Remove(start, end);
    Vma v; v.start = start; v.end = end; v.prot = prot; v.flags = flags;
    vmas_.push_back(std::move(v));
    std::sort(vmas_.begin(), vmas_.end(),
              [](const Vma& a, const Vma& b) { return a.start < b.start; });
    Coalesce();
}

void AddressSpace::Coalesce() {
    for (size_t i = 1; i < vmas_.size();) {
        Vma& prev = vmas_[i - 1];
        const Vma& cur = vmas_[i];
        if (prev.end == cur.start && prev.prot == cur.prot &&
            prev.flags == cur.flags && prev.file == cur.file &&
            (!prev.file || prev.file_offset + prev.end - prev.start == cur.file_offset) && prev.forkable == cur.forkable && prev.shared_backed == cur.shared_backed &&
            (!prev.shared_backed || prev.backing_offset + prev.end - prev.start == cur.backing_offset)) {
            prev.end = cur.end;
            vmas_.erase(vmas_.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
}

bool AddressSpace::FindGapTopDown(size_t len, uintptr_t* out) const {
    uintptr_t top = mmap_top_;
    const uintptr_t bottom = std::max(GuestWindow::start(), brk_);
    for (auto it = vmas_.rbegin(); it != vmas_.rend(); ++it) {
        if (it->end <= bottom) break;
        if (it->end > top) {
            // Overlaps or sits above the search front; pull the front below it.
            if (it->start < top) top = it->start;
            continue;
        }
        if (top - it->end >= len) {
            *out = top - len;
            return true;
        }
        top = it->start;
        if (top <= bottom) return false;
    }
    if (top > bottom && top - bottom >= len) {
        *out = top - len;
        return true;
    }
    return false;
}

long AddressSpace::Mmap(uintptr_t hint, size_t len, int prot, int flags,
                        StubOp* op) {
    *op = {};
    if (len == 0) return -EINVAL;
    if (!RoundLength(len, &len) || len > GuestWindow::kSize) return -ENOMEM;
    if (!ValidProtection(prot)) return -EINVAL;
    const int type = flags & MAP_TYPE;
    if (type != MAP_PRIVATE && type != MAP_SHARED) return -EINVAL;
    if ((flags & MAP_ANONYMOUS) == 0) return -EBADF;

    // Guest placement flags must never be passed through to the host. In
    // particular GROWSDOWN could extend a mapping outside the guest window.
    constexpr int supported = MAP_TYPE | MAP_ANONYMOUS | MAP_FIXED |
        MAP_FIXED_NOREPLACE | MAP_NORESERVE | MAP_STACK | MAP_POPULATE |
        MAP_DENYWRITE | MAP_EXECUTABLE;
    if ((flags & ~supported) != 0) return -EINVAL;
    const bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
    if (fixed && hint % PageSize() != 0) return -EINVAL;

    uintptr_t start;
    if (fixed) {
        start = hint;
        if (start > UINTPTR_MAX - len || !InWindow(start, start + len)) return -ENOMEM;
        if ((flags & MAP_FIXED_NOREPLACE) != 0 && !IsFree(start, start + len)) {
            return -EEXIST;
        }
        // MAP_FIXED replaces whatever was there, which is what the dynamic
        // linker relies on when it drops a library's segments into a span it
        // reserved a moment earlier.
        Remove(start, start + len);
    } else if (hint != 0 && AlignDown(hint) <= UINTPTR_MAX - len &&
               InWindow(AlignDown(hint), AlignDown(hint) + len) &&
               IsFree(AlignDown(hint), AlignDown(hint) + len)) {
        start = AlignDown(hint);
    } else if (!FindGapTopDown(len, &start)) {
        return -ENOMEM;
    }

    Insert(start, start + len, prot, type | MAP_ANONYMOUS);
    *op = {StubOp::kMap, start, len, prot, type | MAP_ANONYMOUS | MAP_FIXED};
    return static_cast<long>(start);
}

long AddressSpace::Mremap(uintptr_t addr, size_t old_len, size_t new_len, int flags, StubOp* op) {
    *op = {};
    if (addr % PageSize() || !new_len || (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP))) return -EINVAL;
    if ((flags & (MREMAP_FIXED | MREMAP_DONTUNMAP)) && !(flags & MREMAP_MAYMOVE)) return -EINVAL;
    // Fixed replacement, duplicate mappings and DONTUNMAP require additional
    // transaction semantics. Never silently treat them as an ordinary move.
    if (!old_len || (flags & (MREMAP_FIXED | MREMAP_DONTUNMAP))) return -EOPNOTSUPP;
    if (!RoundLength(old_len, &old_len) || !RoundLength(new_len, &new_len) ||
        old_len > GuestWindow::kSize || new_len > GuestWindow::kSize ||
        addr > UINTPTR_MAX - old_len || !InWindow(addr, addr + old_len)) return -EINVAL;
    const auto* found = Lookup(addr);
    if (!found || addr + old_len > found->end) return -EFAULT;
    Vma moved = *found;
    if (new_len == old_len) return static_cast<long>(addr);
    uintptr_t destination = addr;
    if (new_len > old_len) {
        // Unused native addresses still hold our PROT_NONE reservation. Move
        // growth into an exact broker-selected gap, never ask Linux to choose
        // an address outside the guest window.
        if (!(flags & MREMAP_MAYMOVE) || moved.shared_backed ||
            !FindGapTopDown(new_len, &destination)) return -ENOMEM;
    }
    if (moved.file && moved.file_offset + (addr - moved.start) > INT64_MAX - new_len) return -EINVAL;
    if (moved.shared_backed) moved.backing_offset += addr - moved.start;
    if (moved.file) moved.file_offset += addr - moved.start;
    moved.start = destination; moved.end = destination + new_len;
    Remove(addr, addr + old_len);
    vmas_.push_back(moved);
    std::sort(vmas_.begin(), vmas_.end(), [](const Vma& a, const Vma& b) { return a.start < b.start; });
    Coalesce();
    op->kind = StubOp::kRemap; op->addr = addr; op->len = old_len;
    op->new_addr = destination; op->new_len = new_len;
    op->flags = destination == addr ? 0 : MREMAP_MAYMOVE | MREMAP_FIXED;
    return static_cast<long>(destination);
}

long AddressSpace::Munmap(uintptr_t addr, size_t len, StubOp* op) {
    *op = {};
    if (addr % PageSize() != 0 || len == 0) return -EINVAL;
    if (!RoundLength(len, &len) || addr > UINTPTR_MAX - len) return -EINVAL;
    if (!InWindow(addr, addr + len)) return -EINVAL;

    Remove(addr, addr + len);
    // Unmapping a range that was never mapped is not an error on Linux, and the
    // stub's own munmap is equally forgiving, so it runs either way.
    *op = {StubOp::kUnmap, addr, len, 0, 0};
    return 0;
}

long AddressSpace::Mprotect(uintptr_t addr, size_t len, int prot, StubOp* op) {
    *op = {};
    if (addr % PageSize() != 0) return -EINVAL;
    if (!ValidProtection(prot)) return -EINVAL;
    if (!RoundLength(len, &len) || addr > UINTPTR_MAX - len) return -ENOMEM;
    if (len == 0) return 0;
    if (!InWindow(addr, addr + len)) return -ENOMEM;
    if (!IsMapped(addr, addr + len)) return -ENOMEM;

    const uintptr_t end = addr + len;
    std::vector<Vma> pieces;
    for (const Vma& v : vmas_) {
        const uintptr_t is = std::max(v.start, addr);
        const uintptr_t ie = std::min(v.end, end);
        if (is < ie) {
            Vma piece = v; piece.start = is; piece.end = ie; piece.prot = prot;
            if (piece.shared_backed) piece.backing_offset += is - v.start;
            if (piece.file) piece.file_offset += is - v.start;
            pieces.push_back(piece);
        }
    }
    Remove(addr, end);
    for (const Vma& piece : pieces) {
        vmas_.push_back(piece);
    }
    std::sort(vmas_.begin(), vmas_.end(), [](const Vma& a, const Vma& b) { return a.start < b.start; });
    Coalesce();

    *op = {StubOp::kProtect, addr, len, prot, 0};
    return 0;
}

long AddressSpace::Brk(uintptr_t addr, StubOp* op) {
    *op = {};
    // brk(2) reports the current break rather than failing, so a libc that asks
    // for the impossible simply learns it did not get it.
    if (addr == 0) return static_cast<long>(brk_);

    const uintptr_t heap_end = brk_start_ < GuestWindow::kLowEnd ? GuestWindow::kLowEnd : mmap_top_;
    if (addr < brk_start_ || addr > heap_end) return static_cast<long>(brk_);
    const uintptr_t want = AlignUp(addr);
    const uintptr_t mapped = AlignUp(brk_);

    if (want > mapped) {
        if (!IsFree(mapped, want)) return static_cast<long>(brk_);
        Insert(mapped, want, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS);
        *op = {StubOp::kMap, mapped, want - mapped, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED};
    } else if (want < mapped) {
        Remove(want, mapped);
        *op = {StubOp::kUnmap, want, mapped - want, 0, 0};
    }
    brk_ = addr;
    return static_cast<long>(brk_);
}

long AddressSpace::Madvise(uintptr_t addr, size_t len, int advice, StubOp* op) {
    *op = {};
    if (addr % PageSize() != 0) return -EINVAL;
    if (!RoundLength(len, &len) || addr > UINTPTR_MAX - len) return -EINVAL;
    // These are the implemented Linux operations, including their observable
    // zeroing/fork semantics. Unknown operations must not falsely succeed.
    switch (advice) {
        case MADV_NORMAL: case MADV_RANDOM: case MADV_SEQUENTIAL:
        case MADV_WILLNEED: case MADV_DONTNEED: case MADV_FREE:
        case MADV_DONTFORK: case MADV_DOFORK:
            break;
        default: return -EINVAL;
    }
    if (len == 0) return 0;
    if (!InWindow(addr, addr + len) || !IsMapped(addr, addr + len)) return -ENOMEM;
    if (advice == MADV_DONTFORK || advice == MADV_DOFORK) {
        std::vector<Vma> pieces;
        for (const auto& v : vmas_) {
            const uintptr_t start = std::max(addr, v.start), end = std::min(addr + len, v.end);
            if (start >= end) { pieces.push_back(v); continue; }
            if (v.start < start) { Vma left = v; left.end = start; pieces.push_back(left); }
            Vma middle = v; middle.start = start; middle.end = end;
            if (middle.shared_backed) middle.backing_offset += start - v.start;
            if (middle.file) middle.file_offset += start - v.start;
            middle.forkable = advice == MADV_DOFORK; pieces.push_back(middle);
            if (v.end > end) {
                Vma right = v; right.start = end;
                if (right.shared_backed) right.backing_offset += end - v.start;
            if (right.file) right.file_offset += end - v.start;
                pieces.push_back(right);
            }
        }
        vmas_ = std::move(pieces); Coalesce();
    }
    *op = {StubOp::kAdvise, addr, len, advice, 0};
    return 0;
}

std::string AddressSpace::DumpMaps() const {
    std::string out;
    char line[128];
    for (const Vma& v : vmas_) {
        snprintf(line, sizeof(line), "%012lx-%012lx %c%c%c%c\n",
                 static_cast<unsigned long>(v.start),
                 static_cast<unsigned long>(v.end),
                 (v.prot & PROT_READ) ? 'r' : '-',
                 (v.prot & PROT_WRITE) ? 'w' : '-',
                 (v.prot & PROT_EXEC) ? 'x' : '-',
                 (v.flags & MAP_SHARED) ? 's' : 'p');
        out += line;
    }
    return out;
}

}  // namespace goblin
