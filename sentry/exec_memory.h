// Getting guest code into executable memory without handing a guest file to the
// Android kernel.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace goblin {

// Whether this device permits mapping a sealed memfd PROT_EXEC. Probed once on
// first call. When false, GuestImage falls back to anonymous RW->RX, which costs
// a private copy per address space but works everywhere.
bool MemfdExecSupported();

// Backing storage for one loaded object, built in three phases: Write the
// segment contents, Seal it, then Map each segment's page range at its own
// protection.
//
// It is one allocation for the whole image rather than one per segment, because
// ELF segments routinely share a page and mapping them separately would let a
// later MAP_FIXED clobber the tail of an earlier one. It also means the sealed
// descriptor can be handed to other guest address spaces later, so every process
// using an object shares one copy of it.
//
// The backing contents are sealed before mapping. Private COW mappings can still
// be changed by guest memory operations; a seal is not a W^X policy.
class GuestImage {
public:
    GuestImage() = default;
    ~GuestImage();
    GuestImage(GuestImage&&) noexcept;
    GuestImage& operator=(GuestImage&&) noexcept;
    GuestImage(const GuestImage&) = delete;
    GuestImage& operator=(const GuestImage&) = delete;

    // `base` must be page-aligned and inside the guest window.
    bool Create(uintptr_t base, size_t size, std::string* err);

    // Offsets are relative to `base`. Anything not written stays zero, which is
    // what a segment's .bss tail needs.
    bool Write(size_t offset, const void* data, size_t len, std::string* err);

    bool Seal(std::string* err);

    // `addr` and `len` are page-aligned and must fall within the image.
    bool MapRange(uintptr_t addr, size_t len, int prot, std::string* err);

    bool sealed_memfd() const { return fd_ >= 0; }

private:
    void Reset();

    uintptr_t base_ = 0;
    size_t size_ = 0;
    int fd_ = -1;         // sealed memfd path
    void* staging_ = nullptr;  // anonymous fallback path
};

}  // namespace goblin
