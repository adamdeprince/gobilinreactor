#include "shared_memory.h"
#include <cerrno>
#include <cstring>
#include <linux/memfd.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace goblin {
bool SharedMemory::Create(std::string* error) {
    int fd = syscall(__NR_memfd_create, "goblin-shared-pages", MFD_CLOEXEC);
    if (fd < 0) { *error = strerror(errno); return false; }
    file = std::make_shared<HostFile>(fd);
    if (ftruncate(fd, kLimit) < 0) { *error = strerror(errno); return false; }
    return true;
}
long SharedMemory::Allocate(size_t bytes) {
    if (!file || bytes > kLimit - used) return -ENOMEM;
    size_t offset = used; used += bytes; return offset;
}
}
