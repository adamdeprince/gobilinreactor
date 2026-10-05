#pragma once
#include "elf_loader.h"
#include "vfs.h"
namespace goblin {
bool LoadProgram(const Vfs& vfs, const std::string& path, LoadedImage* image,
                 std::string* error, uint64_t memory_limit = 512ull << 20);
}
