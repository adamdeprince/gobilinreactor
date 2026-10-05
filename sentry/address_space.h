// The guest's view of its own address space.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace goblin {
struct HostFile;

// Tracks every mapping the guest owns and decides where new ones go.
//
// The sentry does the deciding but cannot do the mapping: creating a mapping in
// another process's address space is not something Linux offers. So this class
// answers "where should it go and is it allowed", and the stub is told to perform
// the actual mmap with MAP_FIXED at the address chosen here. Bookkeeping lives in
// the sentry, where policy belongs; the kernel calls happen in the stub, where the
// address space is.
class AddressSpace {
public:
    struct Vma {
        uintptr_t start = 0;
        uintptr_t end = 0;
        int prot = 0;
        int flags = 0;
        bool shared_backed = false;
        uint64_t backing_offset = 0;
        bool forkable = true;
        std::shared_ptr<HostFile> file;
        uint64_t file_offset = 0;
    };

    // Work the stub must carry out in its own address space for the sentry's
    // decision to become real.
    struct StubOp {
        enum Kind { kNone, kMap, kUnmap, kProtect, kAdvise, kMapFile, kSync, kError, kRemap };
        Kind kind = kNone;
        uintptr_t addr = 0;
        size_t len = 0;
        int prot = 0;
        int flags = 0;
        int fd = -1;
        uint64_t offset = 0;
        uintptr_t new_addr = 0;
        size_t new_len = 0;
    };

    // `image_end` is the first page past the loaded executable; the heap starts
    // there and mmap allocates downward from just below the stack.
    void Init(uintptr_t image_end);

    // Records a mapping that already exists -- the loader's segments and the
    // initial stack -- so nothing is ever placed on top of them.
    void Reserve(uintptr_t start, uintptr_t end, int prot, int flags);
    void SharedBacking(uintptr_t start, size_t size, uint64_t offset);
    void FileBacking(uintptr_t start, size_t size, std::shared_ptr<HostFile> file, uint64_t offset);
    long Msync(uintptr_t addr, size_t len, int flags, StubOp* op);
    const Vma* Lookup(uintptr_t address) const;
    void AfterFork();

    // Each returns a guest address or a negative errno, and fills `op` with what
    // the stub has to do about it. None of them touch guest memory.
    long Mmap(uintptr_t hint, size_t len, int prot, int flags, StubOp* op);
    long Munmap(uintptr_t addr, size_t len, StubOp* op);
    long Mremap(uintptr_t addr, size_t old_len, size_t new_len, int flags, StubOp* op);
    long Mprotect(uintptr_t addr, size_t len, int prot, StubOp* op);
    long Brk(uintptr_t addr, StubOp* op);
    long Madvise(uintptr_t addr, size_t len, int advice, StubOp* op);

    uintptr_t brk() const { return brk_; }
    const std::vector<Vma>& vmas() const { return vmas_; }
    uint64_t MappedBytes() const {
        uint64_t bytes = 0;
        for (const auto& vma : vmas_) bytes += vma.end - vma.start;
        return bytes;
    }

    // The shape /proc/<pid>/maps wants, and a readable debugging aid meanwhile.
    std::string DumpMaps() const;

private:
    // Records a mapping, splitting or merging neighbours as needed.
    void Insert(uintptr_t start, uintptr_t end, int prot, int flags);
    // Removes a range, splitting any mapping it lands in the middle of.
    void Remove(uintptr_t start, uintptr_t end);
    bool IsFree(uintptr_t start, uintptr_t end) const;
    // Whether the whole range is mapped, which munmap tolerates but mprotect
    // does not.
    bool IsMapped(uintptr_t start, uintptr_t end) const;
    bool FindGapTopDown(size_t len, uintptr_t* out) const;
    void Coalesce();

    std::vector<Vma> vmas_;  // sorted by start, never overlapping
    uintptr_t brk_start_ = 0;
    uintptr_t brk_ = 0;
    uintptr_t mmap_top_ = 0;
};

}  // namespace goblin
