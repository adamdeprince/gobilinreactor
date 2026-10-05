#pragma once
#include "vfs.h"
#include <memory>
namespace goblin {
// A retained capability containing exclusively guest shared anonymous pages.
// Confined file mappings use separate, transient broker-transferred descriptors.
struct SharedMemory {
    std::shared_ptr<HostFile> file;
    static constexpr size_t kLimit = 128u << 20;
    size_t used = 0;
    bool Create(std::string* error);
    long Allocate(size_t bytes);
};
}
