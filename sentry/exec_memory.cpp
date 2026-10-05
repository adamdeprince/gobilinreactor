#include "exec_memory.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/memfd.h>

#include "guest_layout.h"

#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif

namespace goblin {
namespace {

std::string Errno(const char* what) {
    return std::string(what) + ": " + strerror(errno);
}

size_t PageSize() {
    static const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return ps;
}

int NewSealableMemfd(const char* name, size_t len, std::string* err) {
    int fd = static_cast<int>(
        syscall(__NR_memfd_create, name, MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd < 0) {
        if (err) *err = Errno("memfd_create");
        return -1;
    }
    if (ftruncate(fd, static_cast<off_t>(len)) != 0) {
        if (err) *err = Errno("ftruncate");
        close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

bool MemfdExecSupported() {
    static const bool supported = [] {
        const size_t ps = PageSize();
        int fd = NewSealableMemfd("goblin-memfd-check", ps, nullptr);
        if (fd < 0) return false;
        const uint32_t ret_insn = 0xd65f03c0u;  // aarch64 `ret`
        bool ok = pwrite(fd, &ret_insn, sizeof(ret_insn), 0) ==
                  static_cast<ssize_t>(sizeof(ret_insn));
        ok = ok && fcntl(fd, F_ADD_SEALS,
                         F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW) == 0;
        if (!ok) {
            close(fd);
            return false;
        }
        void* p = mmap(nullptr, ps, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p == MAP_FAILED) return false;
        munmap(p, ps);
        return true;
    }();
    return supported;
}

GuestImage::~GuestImage() { Reset(); }

void GuestImage::Reset() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    staging_ = nullptr;
    base_ = 0;
    size_ = 0;
}

GuestImage::GuestImage(GuestImage&& other) noexcept
    : base_(other.base_), size_(other.size_), fd_(other.fd_),
      staging_(other.staging_) {
    other.fd_ = -1;
    other.staging_ = nullptr;
    other.base_ = 0;
    other.size_ = 0;
}

GuestImage& GuestImage::operator=(GuestImage&& other) noexcept {
    if (this != &other) {
        Reset();
        base_ = other.base_;
        size_ = other.size_;
        fd_ = other.fd_;
        staging_ = other.staging_;
        other.fd_ = -1;
        other.staging_ = nullptr;
        other.base_ = 0;
        other.size_ = 0;
    }
    return *this;
}

bool GuestImage::Create(uintptr_t base, size_t size, std::string* err) {
    Reset();
    const size_t ps = PageSize();
    if (base % ps != 0 || size % ps != 0 || size == 0) {
        if (err) *err = "image span is not page aligned";
        return false;
    }
    if (!GuestWindow::ContainsRange(base, size)) {
        if (err) *err = "image falls outside the guest window";
        return false;
    }
    base_ = base;
    size_ = size;

    if (MemfdExecSupported()) {
        fd_ = NewSealableMemfd("goblin-image", size, err);
        return fd_ >= 0;
    }

    // Fallback: the mapping itself is the staging area. Contents are written in
    // place while it is still writable, then each range is dropped to its final
    // protection by MapRange.
    void* p = mmap(reinterpret_cast<void*>(GuestWindow::BrokerAddress(base)), size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        if (err) *err = Errno("mapping image");
        Reset();
        return false;
    }
    staging_ = p;
    return true;
}

bool GuestImage::Write(size_t offset, const void* data, size_t len,
                       std::string* err) {
    if (len == 0) return true;
    if (offset > size_ || len > size_ - offset) {
        if (err) *err = "segment contents fall outside the image";
        return false;
    }
    if (fd_ >= 0) {
        if (pwrite(fd_, data, len, static_cast<off_t>(offset)) !=
            static_cast<ssize_t>(len)) {
            if (err) *err = Errno("writing image");
            return false;
        }
        return true;
    }
    memcpy(static_cast<char*>(staging_) + offset, data, len);
    return true;
}

bool GuestImage::Seal(std::string* err) {
    if (fd_ < 0) return true;  // nothing to seal on the fallback path
    if (fcntl(fd_, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
        if (err) *err = Errno("sealing image");
        return false;
    }
    return true;
}

bool GuestImage::MapRange(uintptr_t addr, size_t len, int prot,
                          std::string* err) {
    const size_t ps = PageSize();
    if (addr % ps != 0 || len % ps != 0) {
        if (err) *err = "segment mapping is not page aligned";
        return false;
    }
    if (addr < base_ || len > size_ || addr - base_ > size_ - len) {
        if (err) *err = "segment mapping falls outside the image";
        return false;
    }

    if (fd_ >= 0) {
        // MAP_PRIVATE over a write-sealed memfd is allowed: the seal blocks
        // shared writable mappings, while private pages copy on write, which is
        // exactly the semantics a data segment wants.
        void* got = mmap(reinterpret_cast<void*>(GuestWindow::BrokerAddress(addr)), len, prot,
                         MAP_PRIVATE | MAP_FIXED, fd_,
                         static_cast<off_t>(addr - base_));
        if (got == MAP_FAILED) {
            if (err) *err = Errno("mapping segment");
            return false;
        }
        return true;
    }

    if (mprotect(reinterpret_cast<void*>(GuestWindow::BrokerAddress(addr)), len, prot) != 0) {
        if (err) *err = Errno("mprotect segment");
        return false;
    }
    if ((prot & PROT_EXEC) != 0) {
        char* p = reinterpret_cast<char*>(GuestWindow::BrokerAddress(addr));
        __builtin___clear_cache(p, p + len);
    }
    return true;
}

}  // namespace goblin
