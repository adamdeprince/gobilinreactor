#include "vfs.h"
#include "vfs_metadata.h"
#include "credentials.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace goblin {
HostFile::~HostFile() { if (fd >= 0) close(fd); }
namespace {
constexpr const char* kInodes = ".goblin-inodes";
constexpr const char* kLink = "goblin-inode:";
std::deque<std::string> Parts(const std::string& path) {
    std::deque<std::string> result;
    for (size_t at = 0; at < path.size();) {
        size_t end = path.find('/', at);
        if (end == std::string::npos) end = path.size();
        if (end != at) result.push_back(path.substr(at, end - at));
        at = end + 1;
    }
    return result;
}
std::string Canonical(const std::vector<std::string>& names, const std::string& leaf) {
    std::string result;
    for (const auto& name : names) result += "/" + name;
    if (leaf != ".") result += "/" + leaf;
    return result.empty() ? "/" : result;
}
}

bool Vfs::Mount(const std::string& root, std::string* error) {
    int fd = open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { *error = std::string("opening rootfs: ") + strerror(errno); return false; }
    root_ = std::make_shared<HostFile>(fd);
    int metadata = InitializeRootMetadata(fd);
    if (metadata < 0) { *error = "initializing guest root metadata"; root_.reset(); return false; }
    inodes_.reset(); links_.clear();
    // This directory stores guest inodes only. Its entries are never exposed
    // through guest path traversal or directory listings.
    fd = openat(root_->fd, kInodes, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        inodes_ = std::make_shared<HostFile>(fd);
        int rc = Recover();
        if (rc < 0) { *error = std::string("recovering inode store: ") + strerror(-rc); root_.reset(); return false; }
    } else if (errno != ENOENT) { *error = "opening inode store"; return false; }
    return true;
}

int Vfs::VirtualPath(const std::string& path, bool follow, std::string* out) const {
    Leaf leaf;
    if (Resolve(path, follow, true, &leaf, true) != -EXDEV) return 0;
    *out = leaf.canonical;
    return 1;
}
int Vfs::Resolve(const std::string& path, bool follow, bool missing, Leaf* out, bool detect_proc) const {
    if (!root_) return -ENOENT;
    if (path.empty()) return -ENOENT;
    if (path.size() > 4096) return -ENAMETOOLONG;
    auto pending = Parts(path);
    if (path.back() == '/') pending.emplace_back(".");
    std::vector<std::shared_ptr<HostFile>> dirs{root_};
    std::vector<std::string> names;
    unsigned links = 0;
    while (!pending.empty()) {
        int search = AccessFd(dirs.back()->fd, X_OK); if (search < 0) return search;
        std::string name = pending.front(); pending.pop_front();
        if (name == ".") continue;
        if (name == "..") {
            if (dirs.size() > 1) { dirs.pop_back(); names.pop_back(); }
            continue;
        }
        if (name.size() > 255) return -ENAMETOOLONG;
        if (name == kInodes) return -ENOENT;
        if ((detect_proc || proc_enabled) && names.empty() && name == "proc") {
            out->canonical = "/proc";
            for (const auto& part : pending) out->canonical += "/" + part;
            return -EXDEV;
        }
        int fd = openat(dirs.back()->fd, name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT && missing && pending.empty()) {
                *out = {dirs.back(), name, Canonical(names, name)}; return 0;
            }
            return -errno;
        }
        auto node = std::make_shared<HostFile>(fd);
        struct stat st{};
        if (fstat(fd, &st) != 0) return -errno;
        std::shared_ptr<HostFile> resolved_parent = dirs.back();
        std::string resolved_name = name;
        bool indirect = false;
        if (S_ISLNK(st.st_mode)) {
            char marker[128]; ssize_t length = readlinkat(fd, "", marker, sizeof(marker));
            if (length > static_cast<ssize_t>(strlen(kLink)) && length < static_cast<ssize_t>(sizeof(marker)) &&
                !memcmp(marker, kLink, strlen(kLink))) {
                std::string id(marker + strlen(kLink), length - strlen(kLink));
                if (id.find_first_not_of("0123456789-") != std::string::npos || !inodes_) return -ENOENT;
                int actual = openat(inodes_->fd, id.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
                if (actual < 0) return -errno;
                node = std::make_shared<HostFile>(actual); fd = actual;
                if (fstat(actual, &st) < 0 || S_ISDIR(st.st_mode)) return -EIO;
                resolved_parent = inodes_; resolved_name = id; indirect = true;
            }
        }
        int access = AllowBrokerAccess(fd, st); if (access < 0) return access;
        access = GuestMetadata(fd, &st); if (access < 0) return access;
        if (S_ISLNK(st.st_mode) && (follow || !pending.empty())) {
            if (++links > 40) return -ELOOP;
            char buffer[4097];
            // Empty-path readlink pins the link we just inspected.
            ssize_t n = GuestReadlink(fd, buffer, sizeof(buffer));
            if (n < 0) return n;
            if (n == 0) return -ENOENT;
            if (n > 4096) return -ENAMETOOLONG;
            const std::string target(buffer, n);
            auto extra = Parts(target);
            size_t length = 0;
            for (const auto& part : pending) length += part.size() + 1;
            if (length + target.size() > 4096) return -ENAMETOOLONG;
            if (target[0] == '/') { dirs.resize(1); names.clear(); }
            extra.insert(extra.end(), pending.begin(), pending.end());
            pending = std::move(extra);
            continue;
        }
        if (pending.empty()) {
            *out = {resolved_parent, resolved_name, Canonical(names, name), dirs.back(), name, indirect}; return 0;
        }
        if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
        dirs.push_back(std::move(node)); names.push_back(name);
    }
    *out = {dirs.back(), ".", Canonical(names, ".")};
    return 0;
}

int Vfs::Open(const std::string& path, int flags, unsigned mode, std::string* canonical) const {
    Leaf leaf;
    // O_EXCL must reject a final symlink, including a dangling one.
    const bool follow = !(flags & O_NOFOLLOW) && !((flags & O_CREAT) && (flags & O_EXCL));
    int rc = Resolve(path, follow, flags & O_CREAT, &leaf);
    if (rc < 0) return rc;
    if (canonical) *canonical = leaf.canonical;
    struct stat before{};
    bool create = (flags & O_CREAT) && fstatat(leaf.parent->fd, leaf.name.c_str(), &before, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT;
    if (create) {
        rc = AccessFd(leaf.parent->fd, W_OK | X_OK); if (rc < 0) return rc;
        rc = CheckCreate(); if (rc < 0) return rc;
    } else if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) return -EEXIST;
    else if (!(flags & O_PATH)) {
        HostFile pinned(openat(leaf.parent->fd,leaf.name.c_str(),O_PATH | O_NOFOLLOW | O_CLOEXEC));
        if (pinned.fd < 0) return -errno;
        struct stat st{}; rc = StatFd(pinned.fd,&st); if (rc < 0) return rc;
        if (S_ISLNK(st.st_mode)) return -ELOOP;
        int access = (flags & O_ACCMODE) == O_RDONLY ? R_OK : (flags & O_ACCMODE) == O_WRONLY ? W_OK : R_OK | W_OK;
        if (flags & O_TRUNC) access |= W_OK;
        rc = CheckPermission(st,access); if (rc < 0) return rc;
    }
    int fd = openat(leaf.parent->fd, leaf.name.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, (mode & 07777) | 0600);
    if (fd < 0) return -errno;
    if (create) {
        rc = NewMetadata(fd, leaf.parent->fd, mode);
        if (rc < 0) { close(fd); unlinkat(leaf.parent->fd, leaf.name.c_str(), 0); return rc; }
    }
    struct stat virtual_stat{};
    if (fstat(fd, &virtual_stat) < 0) { int error = -errno; close(fd); return error; }
    rc = GuestMetadata(fd, &virtual_stat);
    if (rc == 0 && (flags & O_TRUNC)) rc = ClearWritePrivilegeBits(fd);
    if (rc < 0 || (S_ISSOCK(virtual_stat.st_mode) && !(flags & O_PATH))) {
        close(fd); return rc < 0 ? rc : -ENXIO;
    }
    return fd;
}
int Vfs::Stat(const std::string& path, struct stat* out, bool follow) const {
    Leaf leaf; int rc = Resolve(path, follow, false, &leaf);
    if (rc < 0) return rc;
    if (fstatat(leaf.parent->fd, leaf.name.c_str(), out, AT_SYMLINK_NOFOLLOW) < 0) return -errno;
    HostFile fd(openat(leaf.parent->fd, leaf.name.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
    if (fd.fd < 0) return -errno;
    rc = GuestMetadata(fd.fd, out); if (rc < 0) return rc;
    AdjustStat(out); return 0;
}
void Vfs::AdjustStat(struct stat* value) const {
    auto found = links_.find({value->st_dev, value->st_ino});
    if (found != links_.end()) value->st_nlink = found->second;
}
int Vfs::StatFd(int fd, struct stat* out) const {
    if (fstat(fd, out) < 0) return -errno;
    int rc = GuestMetadata(fd, out); if (rc < 0) return rc;
    AdjustStat(out); return 0;
}
long Vfs::Readlink(const std::string& path, std::string* out) const {
    Leaf leaf; int rc = Resolve(path, false, false, &leaf);
    if (rc < 0) return rc;
    char buffer[4096];
    HostFile fd(openat(leaf.parent->fd,leaf.name.c_str(),O_PATH | O_NOFOLLOW | O_CLOEXEC));
    if (fd.fd < 0) return -errno;
    ssize_t n = GuestReadlink(fd.fd, buffer, sizeof(buffer));
    if (n < 0) return n;
    out->assign(buffer, n); return n;
}
int Vfs::Access(const std::string& path, int mode, bool follow) const {
    if (mode & ~(R_OK | W_OK | X_OK)) return -EINVAL;
    struct stat st{}; int rc = Stat(path, &st, follow);
    if (rc < 0) return rc;
    return CheckPermission(st, mode);
}
int Vfs::Mkdir(const std::string& path, unsigned mode) const {
    // mkdir("new/") creates the final directory. Ordinary lookup/open still
    // require a trailing slash to resolve to an existing directory.
    std::string directory = path;
    while (directory.size() > 1 && directory.back() == '/') directory.pop_back();
    Leaf leaf; int rc = Resolve(directory, false, true, &leaf);
    if (rc < 0) return rc;
    rc = AccessFd(leaf.parent->fd, W_OK | X_OK); if (rc < 0) return rc;
    rc = CheckCreate(); if (rc < 0) return rc;
    if (mkdirat(leaf.parent->fd, leaf.name.c_str(), (mode & 07777) | 0700) < 0) return -errno;
    HostFile fd(openat(leaf.parent->fd, leaf.name.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    return fd.fd < 0 ? -errno : NewMetadata(fd.fd, leaf.parent->fd, mode, true);
}
int Vfs::ModifyEntry(const Leaf& leaf, bool sticky) const {
    int parent = (leaf.indirect ? leaf.entry_parent : leaf.parent)->fd;
    int rc = AccessFd(parent, W_OK | X_OK); if (rc < 0) return rc;
    const auto& c = CurrentCredentials();
    if (!sticky || !c.fsuid) return 0;
    struct stat dir{}; rc = StatFd(parent,&dir); if (rc < 0) return rc;
    if (!(dir.st_mode & S_ISVTX) || c.fsuid == dir.st_uid) return 0;
    HostFile victim(openat(leaf.parent->fd,leaf.name.c_str(),O_PATH | O_NOFOLLOW | O_CLOEXEC));
    if (victim.fd < 0) return errno == ENOENT ? 0 : -errno;
    struct stat st{}; rc = StatFd(victim.fd,&st); if (rc < 0) return rc;
    return c.fsuid == st.st_uid ? 0 : -EPERM;
}
int Vfs::Unlink(const std::string& path, bool directory) const {
    Leaf leaf; int rc = Resolve(path, false, false, &leaf);
    if (rc < 0) return rc;
    if (leaf.canonical == "/") return -EBUSY;
    rc = ModifyEntry(leaf, true); if (rc < 0) return rc;
    if (leaf.indirect) {
        if (directory) return -ENOTDIR;
        if (unlinkat(leaf.entry_parent->fd, leaf.entry_name.c_str(), 0) < 0) return -errno;
        rc = SyncDirectory(leaf.entry_parent->fd); if (rc < 0) return rc;
        Checkpoint("unlink-entry");
        return DropLink(leaf);
    }
    return unlinkat(leaf.parent->fd, leaf.name.c_str(), directory ? AT_REMOVEDIR : 0) < 0 ? -errno : 0;
}
int Vfs::Rename(const std::string& oldpath, const std::string& newpath) const {
    Leaf oldleaf, newleaf;
    int rc = Resolve(oldpath, false, false, &oldleaf);
    if (rc < 0) return rc;
    rc = Resolve(newpath, false, true, &newleaf);
    if (rc < 0) return rc;
    if (oldleaf.canonical == "/" || newleaf.canonical == "/") return -EBUSY;
    rc = ModifyEntry(oldleaf, true); if (rc < 0) return rc;
    rc = ModifyEntry(newleaf, true); if (rc < 0) return rc;
    if (oldleaf.indirect && newleaf.indirect && oldleaf.name == newleaf.name) return 0;
    int result = renameat((oldleaf.indirect ? oldleaf.entry_parent : oldleaf.parent)->fd,
        (oldleaf.indirect ? oldleaf.entry_name : oldleaf.name).c_str(),
        (newleaf.indirect ? newleaf.entry_parent : newleaf.parent)->fd,
        (newleaf.indirect ? newleaf.entry_name : newleaf.name).c_str());
    if (result < 0) return -errno;
    rc = SyncDirectory((oldleaf.indirect ? oldleaf.entry_parent : oldleaf.parent)->fd); if (rc < 0) return rc;
    rc = SyncDirectory((newleaf.indirect ? newleaf.entry_parent : newleaf.parent)->fd); if (rc < 0) return rc;
    Checkpoint("rename-entry");
    return newleaf.indirect ? DropLink(newleaf) : 0;
}
bool Vfs::ReadFile(const std::string& path, std::vector<uint8_t>* bytes, std::string* error) const {
    int fd = Open(path, O_RDONLY);
    if (fd < 0) { *error = path + ": " + strerror(-fd); return false; }
    HostFile owner(fd); struct stat st{};
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > (64 << 20)) {
        *error = path + ": invalid executable size/type"; return false;
    }
    bytes->resize(st.st_size);
    size_t at = 0;
    while (at < bytes->size()) {
        ssize_t n = pread(fd, bytes->data() + at, bytes->size() - at, at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { *error = path + ": short read"; return false; }
        at += n;
    }
    return true;
}

int Vfs::Symlink(const std::string& target, const std::string& path) const {
    if (target.empty()) return -ENOENT;
    if (target.size() > 4096) return -ENAMETOOLONG;
    if (target.compare(0, strlen(kLink), kLink) == 0) return -EPERM;
    Leaf leaf; int rc = Resolve(path, false, true, &leaf);
    if (rc < 0) return rc;
    rc = CheckCreate(); if (rc < 0) return rc;
    rc = AccessFd(leaf.parent->fd, W_OK | X_OK); if (rc < 0) return rc;
    // Android forbids user xattrs on host symlinks. A marked regular inode
    // stores new guest links so their ownership survives rename and restart.
    HostFile fd(openat(leaf.parent->fd,leaf.name.c_str(),O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,0600));
    if (fd.fd < 0) return -errno;
    rc = NewMetadata(fd.fd,leaf.parent->fd,0777);
    if (rc == 0) rc = SetSymlinkMetadata(fd.fd,target);
    if (rc < 0) unlinkat(leaf.parent->fd,leaf.name.c_str(),0);
    return rc;
}
int Vfs::Link(const std::string& oldpath, const std::string& newpath, bool follow) const {
    Leaf source, target;
    int rc = Resolve(oldpath, follow, false, &source); if (rc < 0) return rc;
    rc = Resolve(newpath, false, true, &target); if (rc < 0) return rc;
    struct stat st{}, existing{};
    if (fstatat(source.parent->fd, source.name.c_str(), &st, AT_SYMLINK_NOFOLLOW) < 0) return -errno;
    if (S_ISDIR(st.st_mode)) return -EPERM;
    if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) return -EOPNOTSUPP;
    if (fstatat(target.parent->fd, target.name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) return -EEXIST;
    if (errno != ENOENT) return -errno;
    rc = ModifyEntry(target, false); if (rc < 0) return rc;
    HostFile source_fd(openat(source.parent->fd,source.name.c_str(),O_PATH | O_NOFOLLOW | O_CLOEXEC));
    if (source_fd.fd < 0) return -errno;
    rc = StatFd(source_fd.fd,&st); if (rc < 0) return rc;
    if (CurrentCredentials().fsuid && CurrentCredentials().fsuid != st.st_uid &&
        (!S_ISREG(st.st_mode) || (st.st_mode & (S_ISUID | S_ISGID)) || CheckPermission(st,R_OK | W_OK))) return -EPERM;
    rc = CheckCreate(source.indirect ? 1 : 3); if (rc < 0) return rc;
    rc = InodeStore(); if (rc < 0) return rc;
    std::string id = source.name;
    uint64_t count = 1;
    if (!source.indirect) {
        id = std::to_string(st.st_dev) + "-" + std::to_string(st.st_ino);
        if (fstatat(inodes_->fd, id.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) return -EIO;
        rc = Promotion(source.canonical, id); if (rc < 0) return rc;
        if (renameat(source.parent->fd, source.name.c_str(), inodes_->fd, id.c_str()) < 0) {
            int error = errno; ClearPromotion(); return -error;
        }
        rc = SyncDirectory(inodes_->fd); if (rc < 0) return rc;
        rc = SyncDirectory(source.parent->fd); if (rc < 0) return rc;
        Checkpoint("promotion-moved");
        std::string marker = std::string(kLink) + id;
        if (symlinkat(marker.c_str(), source.parent->fd, source.name.c_str()) < 0) {
            int error = errno; Recover(); return -error;
        }
        rc = SyncDirectory(source.parent->fd); if (rc < 0) return rc;
        Checkpoint("promotion-source");
        rc = LinkCount(id, 1); if (rc < 0) return rc;
        rc = ClearPromotion(); if (rc < 0) return rc;
    } else {
        auto found = links_.find({st.st_dev, st.st_ino});
        if (found == links_.end()) return -EIO;
        count = found->second;
    }
    if (count >= 1048576) return -EMLINK;
    std::string marker = std::string(kLink) + id;
    if (symlinkat(marker.c_str(), target.parent->fd, target.name.c_str()) < 0) return -errno;
    rc = SyncDirectory(target.parent->fd); if (rc < 0) return rc;
    Checkpoint("link-entry");
    rc = LinkCount(id, count + 1);
    if (rc < 0) Recover();
    return rc;
}
int Vfs::InodeStore() const {
    if (inodes_) return 0;
    if (mkdirat(root_->fd, kInodes, 0700) < 0 && errno != EEXIST) return -errno;
    int fd = openat(root_->fd, kInodes, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    inodes_ = std::make_shared<HostFile>(fd); return SyncDirectory(root_->fd);
}
int Vfs::LinkCount(const std::string& id, uint64_t count) const {
    const std::string temporary = id + ".new", final = id + ".count";
    HostFile record(openat(inodes_->fd, temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (record.fd < 0) return -errno;
    if (write(record.fd, &count, sizeof(count)) != sizeof(count) || fsync(record.fd) < 0) return -EIO;
    if (renameat(inodes_->fd, temporary.c_str(), inodes_->fd, final.c_str()) < 0) return -errno;
    struct stat st{};
    if (fstatat(inodes_->fd, id.c_str(), &st, AT_SYMLINK_NOFOLLOW) < 0) return -errno;
    links_[{st.st_dev, st.st_ino}] = count;
    return SyncDirectory(inodes_->fd);
}
int Vfs::DropLink(const Leaf& leaf) const {
    struct stat st{};
    if (fstatat(inodes_->fd, leaf.name.c_str(), &st, AT_SYMLINK_NOFOLLOW) < 0) return -errno;
    auto found = links_.find({st.st_dev, st.st_ino});
    if (found == links_.end() || !found->second) return -EIO;
    if (found->second > 1) return LinkCount(leaf.name, found->second - 1);
    if (unlinkat(inodes_->fd, leaf.name.c_str(), 0) < 0) return -errno;
    unlinkat(inodes_->fd, (leaf.name + ".count").c_str(), 0);
    links_.erase(found); return SyncDirectory(inodes_->fd);
}
int Vfs::Chmod(const std::string& path, unsigned mode) const {
    int fd = Open(path, O_PATH);
    if (fd < 0) return fd;
    HostFile owner(fd);
    return ChmodFd(fd, mode);
}
int Vfs::Times(const std::string& path, const struct timespec* times, bool follow) const {
    Leaf leaf; int rc = Resolve(path, follow, false, &leaf);
    if (rc < 0) return rc;
    HostFile fd(openat(leaf.parent->fd,leaf.name.c_str(),O_PATH | O_NOFOLLOW | O_CLOEXEC));
    if (fd.fd < 0) return -errno;
    return TimesFd(fd.fd,times);
}

int Vfs::TimesFd(int fd, const timespec* times) const {
    struct stat st{}; int rc = StatFd(fd,&st); if (rc < 0) return rc;
    if (times && times[0].tv_nsec == UTIME_OMIT && times[1].tv_nsec == UTIME_OMIT) return 0;
    bool now = !times || (times[0].tv_nsec == UTIME_NOW && times[1].tv_nsec == UTIME_NOW);
    if (CurrentCredentials().fsuid && CurrentCredentials().fsuid != st.st_uid) {
        if (!now) return -EPERM;
        rc = CheckPermission(st,W_OK); if (rc < 0) return rc;
    }
    return syscall(__NR_utimensat,fd,"",times,AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) < 0 ? -errno : 0;
}

void Vfs::ReleaseLocks(int owner, int fd) {
    struct stat st{};
    if (fd >= 0 && fstat(fd, &st) < 0) return;
    locks_.erase(std::remove_if(locks_.begin(), locks_.end(), [&](const auto& lock) {
        return lock.owner == owner && (fd < 0 || (lock.device == st.st_dev && lock.inode == st.st_ino));
    }), locks_.end());
}
long Vfs::Lock(int fd, int owner, int command, struct flock* value) {
    if (value->l_type != F_RDLCK && value->l_type != F_WRLCK && value->l_type != F_UNLCK) return -EINVAL;
    struct stat st{}; if (fstat(fd, &st) < 0) return -errno;
    int64_t base = 0;
    if (value->l_whence == SEEK_CUR) { base = lseek(fd, 0, SEEK_CUR); if (base < 0) return -errno; }
    else if (value->l_whence == SEEK_END) base = st.st_size;
    else if (value->l_whence != SEEK_SET) return -EINVAL;
    __int128 start = __int128(base) + value->l_start;
    __int128 end = value->l_len ? start + value->l_len - 1 : INT64_MAX;
    if (value->l_len < 0) { end = start - 1; start += value->l_len; }
    if (start < 0 || end < start || end > INT64_MAX) return -EINVAL;
    for (const auto& lock : locks_) {
        if (lock.device != st.st_dev || lock.inode != st.st_ino || lock.owner == owner ||
            lock.end < start || end < lock.start || (lock.type == F_RDLCK && value->l_type == F_RDLCK) ||
            value->l_type == F_UNLCK) continue;
        if (command == F_GETLK) {
            value->l_type = lock.type; value->l_whence = SEEK_SET; value->l_start = lock.start;
            value->l_len = lock.end == INT64_MAX ? 0 : lock.end - lock.start + 1; value->l_pid = lock.owner; return 0;
        }
        return -EAGAIN;
    }
    if (command == F_GETLK) { value->l_type = F_UNLCK; return 0; }
    std::vector<LockRange> next;
    for (auto lock : locks_) {
        if (lock.owner != owner || lock.device != st.st_dev || lock.inode != st.st_ino || lock.end < start || end < lock.start) {
            next.push_back(lock); continue;
        }
        if (lock.start < start) { auto left = lock; left.end = start - 1; next.push_back(left); }
        if (lock.end > end) { lock.start = end + 1; next.push_back(lock); }
    }
    if (value->l_type != F_UNLCK) next.push_back({st.st_dev, st.st_ino, owner, value->l_type, int64_t(start), int64_t(end)});
    locks_ = std::move(next); return 0;
}

bool InstallRootfs(const uint8_t* data, size_t size, const std::string& directory, std::string* error) {
    auto fail = [&](const char* why) { *error = why; return false; };
    if (size < 12 || memcmp(data, "GOBLINFS", 8)) return fail("invalid rootfs header");
    uint32_t count; memcpy(&count, data + 8, 4);
    if (count > 262144) return fail("rootfs entry limit exceeded");
    // The caller uses a new directory for each run. Refuse to merge into an
    // existing tree where a previous guest might have changed the paths.
    if (mkdir(directory.c_str(), 0700) != 0) return fail("creating fresh rootfs failed");
    HostFile root(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (root.fd < 0) return fail("opening rootfs failed");
    size_t at = 12;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t length, mode; uint64_t bytes;
        if (size - at < 16) return fail("truncated rootfs entry");
        memcpy(&length, data + at, 4); memcpy(&mode, data + at + 4, 4);
        memcpy(&bytes, data + at + 8, 8); at += 16;
        if (!length || length > 4096 || length > size - at || bytes > size - at - length)
            return fail("invalid rootfs entry length");
        std::string path(reinterpret_cast<const char*>(data + at), length); at += length;
        if (path[0] == '/' || path.find('\0') != std::string::npos) return fail("invalid rootfs path");
        auto parts = Parts(path);
        int parent = dup(root.fd); HostFile owner(parent);
        for (size_t j = 0; j < parts.size(); ++j) {
            if (parts[j] == "." || parts[j] == "..") return fail("unsafe rootfs path");
            if (j + 1 == parts.size()) break;
            int next = openat(owner.fd, parts[j].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0) return fail("unsafe rootfs parent");
            close(owner.fd); owner.fd = next;
        }
        if (parts.empty()) return fail("empty rootfs path");
        const char* leaf = parts.back().c_str();
        if (S_ISDIR(mode)) {
            if (bytes || mkdirat(owner.fd, leaf, 0700) < 0) return fail("rootfs mkdir failed");
            HostFile child(openat(owner.fd, leaf, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (child.fd < 0 || SetGuestMetadata(child.fd, mode, 0, 0) < 0) return fail("rootfs directory metadata failed");
        } else if (S_ISREG(mode)) {
            HostFile file(openat(owner.fd, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, (mode & 07777) | 0600));
            if (file.fd < 0) return fail("rootfs create failed");
            if (SetGuestMetadata(file.fd, mode, 0, 0) < 0) return fail("rootfs file metadata failed");
            size_t written = 0;
            while (written < bytes) {
                ssize_t n = write(file.fd, data + at + written, bytes - written);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return fail("rootfs write failed");
                written += n;
            }
        } else if (S_ISLNK(mode)) {
            std::string target(reinterpret_cast<const char*>(data + at), bytes);
            if (target.empty() || target.size() > 4096 || target.find('\0') != std::string::npos ||
                symlinkat(target.c_str(), owner.fd, leaf) < 0) return fail("rootfs symlink failed");
        } else return fail("unsupported rootfs entry");
        at += bytes;
    }
    return at == size || fail("trailing rootfs data");
}
}  // namespace goblin
