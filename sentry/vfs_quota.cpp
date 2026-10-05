#include "vfs.h"
#include "vfs_metadata.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <unistd.h>

namespace goblin {
namespace {
// Logical sizes are charged even for sparse files. The per-inode allowance
// covers directory entries, guest metadata and hard-link count records.
uint64_t Charge(const struct stat& st, uint64_t size) {
    return 8192 + (S_ISREG(st.st_mode) ? (size + 4095) & ~4095ull : 0);
}
}
int Vfs::ScanDisk() const {
    std::map<std::pair<dev_t, ino_t>, uint64_t> files;
    auto add = [&](int fd) -> int {
        struct stat st{}; if (fstat(fd, &st) < 0) return -errno;
        files[{st.st_dev, st.st_ino}] = Charge(st, std::max<off_t>(0, st.st_size)); return 0;
    };
    size_t entries = 0;
    std::function<int(int, unsigned)> visit = [&](int fd, unsigned depth) -> int {
        if (depth > 256 || ++entries > 1000000) return -EFBIG;
        int rc = add(fd); if (rc < 0) return rc;
        struct stat st{}; if (fstat(fd, &st) < 0) return -errno;
        if (!S_ISDIR(st.st_mode)) return 0;
        rc = AllowBrokerAccess(fd, st); if (rc < 0) return rc;
        int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (copy < 0) return -errno;
        DIR* dir = fdopendir(copy); if (!dir) { close(copy); return -errno; }
        while (auto* entry = readdir(dir)) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            HostFile node(openat(fd, entry->d_name, O_PATH | O_NOFOLLOW | O_CLOEXEC));
            if (node.fd < 0) { rc = -errno; break; }
            rc = visit(node.fd, depth + 1); if (rc < 0) break;
        }
        closedir(dir); return rc;
    };
    if (root_) { int rc = visit(root_->fd, 0); if (rc < 0) return rc; }
    // Unlinked files still consume the quota while descriptors or VMAs retain
    // them. Rescanning must not make a deleted-but-open file free.
    for (auto it = disk_open_.begin(); it != disk_open_.end();) {
        if (auto file = it->lock()) { int rc = add(file->fd); if (rc < 0) return rc; ++it; }
        else it = disk_open_.erase(it);
    }
    disk_used_ = 0;
    for (const auto& file : files) disk_used_ += file.second;
    disk_files_ = std::move(files); return 0;
}
int Vfs::SetDiskLimit(uint64_t bytes) {
    disk_limit_ = bytes; int rc = ScanDisk();
    return rc < 0 ? rc : disk_used_ > disk_limit_ ? -ENOSPC : 0;
}
int Vfs::StatFs(int fd, struct statfs* out) const {
    if (fstatfs(fd, out) < 0) return -errno;
    if (!root_ || disk_limit_ == UINT64_MAX) return 0;
    struct stat file{}, root{};
    if (fstat(fd, &file) < 0 || fstat(root_->fd, &root) < 0) return -errno;
    // Pipes, sockets and host terminal devices retain their own filesystem
    // statistics. Only the private root's filesystem consumes this quota.
    if (file.st_dev != root.st_dev) return 0;
    int rc = ScanDisk(); if (rc < 0) return rc;
    uint64_t unit = out->f_frsize > 0 ? out->f_frsize : out->f_bsize;
    if (!unit) return -EIO;
    uint64_t remaining = disk_used_ < disk_limit_ ? disk_limit_ - disk_used_ : 0;
    // APT and df must see the same logical limit as writes, while still
    // respecting less available space on the Android backing filesystem.
    out->f_blocks = std::min<uint64_t>(out->f_blocks, disk_limit_ / unit);
    out->f_bfree = std::min<uint64_t>(out->f_bfree, remaining / unit);
    out->f_bavail = std::min<uint64_t>(out->f_bavail, remaining / unit);
    return 0;
}
int Vfs::Sync() const {
    if (!root_) return 0;
    size_t entries = 0;
    std::function<int(int, unsigned)> visit = [&](int fd, unsigned depth) -> int {
        if (depth > 256 || ++entries > 1000000) return -EFBIG;
        struct stat st{}; if (fstat(fd, &st) < 0) return -errno;
        if (!S_ISDIR(st.st_mode)) return S_ISREG(st.st_mode) && fsync(fd) < 0 ? -errno : 0;
        int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (copy < 0) return -errno;
        DIR* dir = fdopendir(copy); if (!dir) { close(copy); return -errno; }
        int rc = 0;
        while (auto* entry = readdir(dir)) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            struct stat child{};
            if (fstatat(fd, entry->d_name, &child, AT_SYMLINK_NOFOLLOW) < 0) { rc = -errno; break; }
            if (S_ISLNK(child.st_mode)) continue;
            HostFile node(openat(fd, entry->d_name, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
            if (node.fd < 0) { rc = -errno; break; }
            rc = visit(node.fd, depth + 1); if (rc < 0) break;
        }
        if (rc == 0 && fsync(copy) < 0) rc = -errno;
        closedir(dir); return rc;
    };
    return visit(root_->fd, 0);
}
void Vfs::Track(const std::shared_ptr<HostFile>& file) const {
    struct stat st{};
    if (fstat(file->fd, &st) == 0 && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) disk_open_.push_back(file);
    if (disk_open_.size() > 1024) disk_open_.erase(std::remove_if(disk_open_.begin(), disk_open_.end(),
        [](const auto& f) { return f.expired(); }), disk_open_.end());
}
int Vfs::CheckCreate(unsigned count) const {
    uint64_t bytes = uint64_t(count) * 8192;
    if (disk_used_ > disk_limit_ || bytes > disk_limit_ - disk_used_) {
        int rc = ScanDisk(); if (rc < 0) return rc;
        if (disk_used_ > disk_limit_ || bytes > disk_limit_ - disk_used_) return -ENOSPC;
    }
    // Conservative until the inode exists; a later pressure rescan reconciles
    // failed creations, deleted files and temporary hard-link bookkeeping.
    disk_used_ += bytes; return 0;
}
int Vfs::CheckGrowth(int fd, uint64_t size) const {
    struct stat st{}; if (fstat(fd, &st) < 0) return -errno;
    if (!S_ISREG(st.st_mode)) return 0;
    if (size > INT64_MAX - 8192) return -EFBIG;
    const auto key = std::make_pair(st.st_dev, st.st_ino);
    const uint64_t desired = Charge(st, size);
    uint64_t old = disk_files_.count(key) ? disk_files_.at(key) : 0;
    uint64_t growth = desired > old ? desired - old : 0;
    if (disk_used_ > disk_limit_ || growth > disk_limit_ - disk_used_) {
        int rc = ScanDisk(); if (rc < 0) return rc;
        old = disk_files_.count(key) ? disk_files_.at(key) : 0;
        growth = desired > old ? desired - old : 0;
        if (disk_used_ > disk_limit_ || growth > disk_limit_ - disk_used_) return -ENOSPC;
    }
    disk_used_ += growth; disk_files_[key] = std::max(old, desired); return 0;
}
}
