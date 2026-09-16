// Loading a guest ELF image into the guest window.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "exec_memory.h"

namespace goblin {

struct LoadedImage {
    uintptr_t bias = 0;         // added to every p_vaddr; zero for ET_EXEC
    uintptr_t entry = 0;        // where to start the guest
    uintptr_t image_start = 0;  // page-aligned span actually mapped
    uintptr_t image_end = 0;
    uintptr_t exec_start = 0;   // union of executable segments, for the filter
    uintptr_t exec_end = 0;
    uintptr_t brk = 0;          // first free page past the image
    GuestImage storage;
};

// Parses and maps `file`. The bytes are read as ordinary data -- the kernel is
// never asked to execute the file, which is the entire point.
bool LoadElf(const uint8_t* file, size_t size, LoadedImage* out, std::string* err);

}  // namespace goblin
