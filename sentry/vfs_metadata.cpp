#include "vfs_metadata.h"
#include "vfs.h"
#include "credentials.h"
#include <cerrno>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <string>
#include <sys/xattr.h>
#include <unistd.h>

namespace goblin {
namespace {
constexpr const char* kMetadata = "user.goblin.metadata.v1";
constexpr const char* kSymlink = "user.goblin.symlink.v1";
constexpr const char* kSocket = "user.goblin.socket.v1";
struct Metadata { uint32_t magic, mode, uid, gid; };
constexpr uint32_t kMagic = 0x474d4431;
std::string Pinned(int fd) { return "/proc/self/fd/" + std::to_string(fd); }
}
int InitializeRootMetadata(int fd) {
    Metadata data{};
    if (getxattr(Pinned(fd).c_str(), kMetadata, &data, sizeof(data)) < 0) {
        if (errno != ENODATA) return -errno;
        return SetGuestMetadata(fd,0755,0,0);
    }
    return 0;
}
int GuestMetadata(int fd, struct stat* value) {
    // Following a symlink via proc would escape the pinned inode. Never do so.
    value->st_uid = value->st_gid = 0;
    if (!S_ISREG(value->st_mode) && !S_ISDIR(value->st_mode)) return 0;
    Metadata data{};
    ssize_t n = getxattr(Pinned(fd).c_str(), kMetadata, &data, sizeof(data));
    if (n < 0 && errno != ENODATA) return -errno;
    if (n >= 0) {
        if (n != sizeof(data) || data.magic != kMagic || (data.mode & ~07777u)) return -EIO;
        value->st_mode = (value->st_mode & S_IFMT) | data.mode;
        value->st_uid = data.uid; value->st_gid = data.gid;
    }
    ssize_t link_size = getxattr(Pinned(fd).c_str(), kSymlink, nullptr, 0);
    if (link_size < 0 && errno != ENODATA) return -errno;
    if (link_size >= 0) {
        if (!link_size || link_size > 4096 || !S_ISREG(value->st_mode)) return -EIO;
        value->st_mode = S_IFLNK | 0777; value->st_size = link_size;
        return 0;
    }
    uint32_t socket = 0;
    n = getxattr(Pinned(fd).c_str(), kSocket, &socket, sizeof(socket));
    if (n < 0 && errno != ENODATA) return -errno;
    if (n >= 0) {
        if (n != sizeof(socket) || socket != 0x47534f31 || !S_ISREG(value->st_mode)) return -EIO;
        value->st_mode = S_IFSOCK | (value->st_mode & 07777);
    }
    return 0;
}
long GuestReadlink(int fd, char* buffer, size_t size) {
    struct stat st{}; if (fstat(fd, &st) < 0) return -errno;
    if (S_ISLNK(st.st_mode)) { long n = readlinkat(fd, "", buffer, size); return n < 0 ? -errno : n; }
    char target[4096];
    long n = getxattr(Pinned(fd).c_str(), kSymlink, target, sizeof(target));
    if (n < 0) return errno == ENODATA ? -EINVAL : -errno;
    size_t count = std::min(size, size_t(n)); memcpy(buffer, target, count); return count;
}
int SetSymlinkMetadata(int fd, const std::string& target) {
    return setxattr(Pinned(fd).c_str(), kSymlink, target.data(), target.size(), 0) < 0 ? -errno : 0;
}
int SetSocketMetadata(int fd) {
    const uint32_t marker = 0x47534f31;
    return setxattr(Pinned(fd).c_str(), kSocket, &marker, sizeof(marker), 0) < 0 ? -errno : 0;
}
int SetGuestMetadata(int fd, unsigned mode, unsigned uid, unsigned gid) {
    struct stat st{};
    if (fstat(fd, &st) < 0) return -errno;
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
        return (uid == 0 || uid == UINT32_MAX) && (gid == 0 || gid == UINT32_MAX) ? 0 : -EOPNOTSUPP;
    int rc = GuestMetadata(fd, &st); if (rc < 0) return rc;
    Metadata data{kMagic, mode == UINT32_MAX ? unsigned(st.st_mode & 07777) : mode & 07777,
                  uid == UINT32_MAX ? st.st_uid : uid, gid == UINT32_MAX ? st.st_gid : gid};
    const std::string pinned = Pinned(fd);
    if (setxattr(pinned.c_str(), kMetadata, &data, sizeof(data), 0) < 0) return -errno;
    // Preserve execute bits for Android's executable-file mapping policy, while
    // ensuring virtual root can access mode-000 files and directories.
    return chmod(pinned.c_str(), (data.mode & 0777) | (S_ISDIR(st.st_mode) ? 0700 : 0600)) < 0 ? -errno : 0;
}
int AllowBrokerAccess(int fd, const struct stat& value) {
    unsigned needed = S_ISDIR(value.st_mode) ? 0700 : S_ISREG(value.st_mode) ? 0600 : 0;
    if ((value.st_mode & needed) == needed) return 0;
    return SetGuestMetadata(fd, UINT32_MAX, UINT32_MAX, UINT32_MAX);
}
int ClearWritePrivilegeBits(int fd) {
    if (!CurrentCredentials().fsuid) return 0;
    struct stat st{};
    if (fstat(fd, &st) < 0) return -errno;
    if (!S_ISREG(st.st_mode)) return 0;
    int rc = GuestMetadata(fd, &st); if (rc < 0) return rc;
    unsigned mask = S_ISUID | ((st.st_mode & S_IXGRP) ? S_ISGID : 0);
    return (st.st_mode & mask) ? SetGuestMetadata(fd, st.st_mode & ~mask, UINT32_MAX, UINT32_MAX) : 0;
}
int SyncDirectory(int fd) {
    HostFile readable(openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    return readable.fd < 0 || fsync(readable.fd) < 0 ? -errno : 0;
}
int Vfs::AccessFd(int fd, int mode) const {
    struct stat st{}; int rc = StatFd(fd, &st); return rc < 0 ? rc : CheckPermission(st, mode);
}
int Vfs::NewMetadata(int fd, int parent, unsigned mode, bool directory) const {
    const auto& c = CurrentCredentials(); struct stat st{};
    int rc = StatFd(parent, &st); if (rc < 0) return rc;
    uint32_t gid = st.st_mode & S_ISGID ? st.st_gid : c.fsgid;
    if (directory && (st.st_mode & S_ISGID)) mode |= S_ISGID;
    if (!directory && c.fsuid && !c.InGroup(gid)) mode &= ~S_ISGID;
    return SetGuestMetadata(fd, mode, c.fsuid, gid);
}
int Vfs::ChmodFd(int fd, unsigned mode) const {
    struct stat st{}; int rc = StatFd(fd,&st); if (rc < 0) return rc;
    const auto& c = CurrentCredentials();
    if (c.fsuid && c.fsuid != st.st_uid) return -EPERM;
    if (c.fsuid && !c.InGroup(st.st_gid)) mode &= ~S_ISGID;
    return SetGuestMetadata(fd, mode, UINT32_MAX, UINT32_MAX);
}
int Vfs::ChownFd(int fd, unsigned uid, unsigned gid) const {
    struct stat st{}; int rc = StatFd(fd,&st); if (rc < 0) return rc;
    const auto& c = CurrentCredentials();
    if (c.fsuid && (c.fsuid != st.st_uid || (uid != UINT32_MAX && uid != st.st_uid) ||
        (gid != UINT32_MAX && gid != st.st_gid && !c.InGroup(gid)))) return -EPERM;
    unsigned mode = st.st_mode & 07777;
    if (!S_ISDIR(st.st_mode) && (uid != UINT32_MAX || gid != UINT32_MAX)) mode &= ~(S_ISUID | S_ISGID);
    return SetGuestMetadata(fd, mode, uid, gid);
}
int Vfs::Chown(const std::string& path, unsigned uid, unsigned gid, bool follow) const {
    int fd = Open(path, O_PATH | (follow ? 0 : O_NOFOLLOW));
    if (fd < 0) return fd;
    HostFile owner(fd); return ChownFd(fd, uid, gid);
}
int Vfs::PathFd(int fd, std::string* path) const {
    if (!root_) return -ENOENT;
    char base[8192], node[8192];
    ssize_t a = readlink(Pinned(root_->fd).c_str(), base, sizeof(base));
    ssize_t b = readlink(Pinned(fd).c_str(), node, sizeof(node));
    if (a <= 0 || b < a || a == sizeof(base) || b == sizeof(node)) return -ENOENT;
    const std::string root(base, a), target(node, b);
    if (target != root && target.compare(0, root.size() + 1, root + "/") != 0) return -EXDEV;
    *path = target.size() == root.size() ? "/" : target.substr(root.size());
    // Verify identity: an unlinked directory or a replacement with its old name
    // must never redirect a dirfd or cwd into a different inode.
    int resolved = Open(*path, O_PATH | O_DIRECTORY);
    if (resolved < 0) return resolved;
    HostFile owner(resolved); struct stat left{}, right{};
    if (fstat(fd, &left) < 0 || fstat(resolved, &right) < 0) return -errno;
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino ? 0 : -ENOENT;
}
}
