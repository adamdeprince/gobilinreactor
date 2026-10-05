// Loading a guest ELF image into the guest window.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <vector>

#include "exec_memory.h"

namespace goblin {

struct LoadedImage {
    // Kept so the address space starts out knowing exactly what is mapped and
    // at what protection, rather than approximating the image as one blob.
    struct Segment {
        uintptr_t start = 0;
        uintptr_t end = 0;
        int prot = 0;
    };
    std::vector<Segment> segments;

    uintptr_t bias = 0;         // added to every p_vaddr; zero for ET_EXEC
    uintptr_t entry = 0;        // where to start the guest
    uintptr_t image_start = 0;  // page-aligned span actually mapped
    uintptr_t image_end = 0;
    uintptr_t exec_start = 0;   // union of executable segments, for the filter
    uintptr_t exec_end = 0;
    uintptr_t brk = 0;          // first free page past the image
    uintptr_t phdr = 0;
    uint16_t phnum = 0;
    uintptr_t interpreter_base = 0;
    uintptr_t start_entry = 0;
    std::string interpreter;
    GuestImage storage;
    std::vector<GuestImage> interpreter_storage;
};

// Parses and maps `file`. The bytes are read as ordinary data -- the kernel is
// never asked to execute the file, which is the entire point.
bool LoadElf(const uint8_t* file, size_t size, LoadedImage* out, std::string* err,
             uintptr_t placement = 0, uint64_t memory_limit = 512ull << 20);

}  // namespace goblin
