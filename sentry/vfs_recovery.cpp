#include "vfs.h"
#include "vfs_metadata.h"
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <set>
#include <unistd.h>

namespace goblin {
namespace {
constexpr const char* kJournal = "promotion";
constexpr const char* kMarker = "goblin-inode:";
bool Id(const std::string& id) {
    return !id.empty() && id.size() < 64 && id.find_first_not_of("0123456789-") == std::string::npos;
}
int WriteAll(int fd, const std::string& bytes) {
    size_t at = 0;
    while (at < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + at, bytes.size() - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? -errno : -EIO;
        at += n;
    }
    return fsync(fd) < 0 ? -errno : 0;
}
}
int Vfs::Promotion(const std::string& path, const std::string& id) const {
    HostFile fd(openat(inodes_->fd, "promotion.new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (fd.fd < 0) return -errno;
    // NUL separators also support newlines in Linux filenames.
    int rc = WriteAll(fd.fd, id + '\0' + path + '\0'); if (rc < 0) return rc;
    if (renameat(inodes_->fd, "promotion.new", inodes_->fd, kJournal) < 0) return -errno;
    rc = SyncDirectory(inodes_->fd); if (rc == 0) Checkpoint("promotion-journal");
    return rc;
}
int Vfs::ClearPromotion() const {
    if (unlinkat(inodes_->fd, kJournal, 0) < 0 && errno != ENOENT) return -errno;
    return SyncDirectory(inodes_->fd);
}
int Vfs::Recover() const {
    // A first hard link moves a native inode into our private store. Complete
    // that promotion before rebuilding counts. All other namespace changes
    // are native atomic rename/symlink/unlink operations, with counts a cache.
    HostFile journal(openat(inodes_->fd, kJournal, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (journal.fd >= 0) {
        char data[8192]; ssize_t n = read(journal.fd, data, sizeof(data));
        if (n <= 2 || n == sizeof(data) || data[n - 1] != '\0') return -EIO;
        size_t split = strnlen(data, n);
        if (split + 2 >= size_t(n)) return -EIO;
        std::string id(data, split), path(data + split + 1, n - split - 2);
        if (!Id(id) || path.empty() || path[0] != '/' || path.find('\0') != std::string::npos) return -EIO;
        Leaf leaf;
        int rc = Resolve(path, false, true, &leaf); if (rc < 0) return rc;
        struct stat stored{}, original{};
        bool backing = fstatat(inodes_->fd, id.c_str(), &stored, AT_SYMLINK_NOFOLLOW) == 0;
        if (!backing && errno != ENOENT) return -errno;
        bool present = fstatat(leaf.parent->fd, leaf.name.c_str(), &original, AT_SYMLINK_NOFOLLOW) == 0;
        if (!present && errno != ENOENT) return -errno;
        if (backing && !present) {
            if (symlinkat((std::string(kMarker) + id).c_str(), leaf.parent->fd, leaf.name.c_str()) < 0) return -errno;
            rc = SyncDirectory(leaf.parent->fd); if (rc < 0) return rc;
        } else if (backing && (!leaf.indirect || leaf.name != id)) return -EIO;
        else if (!backing && !present) return -EIO;
        rc = ClearPromotion(); if (rc < 0) return rc;
    } else if (errno != ENOENT) return -errno;

    std::map<std::string, uint64_t> counts;
    size_t entries = 0;
    std::function<int(int, unsigned)> scan = [&](int fd, unsigned depth) -> int {
        if (depth > 256) return -ELOOP;
        int readable = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (readable < 0) return -errno;
        DIR* dir = fdopendir(readable);
        if (!dir) { close(readable); return -errno; }
        int rc = 0;
        while (auto* entry = readdir(dir)) {
            std::string name = entry->d_name;
            if (name == "." || name == ".." || name == ".goblin-inodes") continue;
            if (++entries > 1000000) { rc = -EFBIG; break; }
            HostFile node(openat(fd, name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
            struct stat st{};
            if (node.fd < 0 || fstat(node.fd, &st) < 0) { rc = -errno; break; }
            rc = AllowBrokerAccess(node.fd, st); if (rc < 0) break;
            if (S_ISDIR(st.st_mode)) { rc = scan(node.fd, depth + 1); if (rc < 0) break; }
            else if (S_ISLNK(st.st_mode)) {
                char target[128]; ssize_t n = readlinkat(node.fd, "", target, sizeof(target));
                if (n > ssize_t(strlen(kMarker)) && n < ssize_t(sizeof(target)) && !memcmp(target, kMarker, strlen(kMarker))) {
                    std::string id(target + strlen(kMarker), n - strlen(kMarker));
                    struct stat inode{};
                    if (!Id(id) || fstatat(inodes_->fd, id.c_str(), &inode, AT_SYMLINK_NOFOLLOW) < 0 || S_ISDIR(inode.st_mode)) {
                        rc = -EIO; break;
                    }
                    ++counts[id];
                }
            }
        }
        closedir(dir); return rc;
    };
    int rc = scan(root_->fd, 0); if (rc < 0) return rc;
    links_.clear();
    // Rebuild even if a process died after publishing a link but before its
    // count, or after unlink/overwrite but before dropping the old count.
    for (const auto& item : counts) { rc = LinkCount(item.first, item.second); if (rc < 0) return rc; }
    int copy = openat(inodes_->fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (copy < 0) return -errno;
    DIR* dir = fdopendir(copy); if (!dir) { close(copy); return -errno; }
    while (auto* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string id = name.substr(0, name.find('.'));
        if (Id(id) && !counts.count(id)) {
            if (unlinkat(inodes_->fd, name.c_str(), 0) < 0) { rc = -errno; break; }
        } else if (name == "promotion.new" || (name.size() > 4 && name.substr(name.size() - 4) == ".new")) {
            if (unlinkat(inodes_->fd, name.c_str(), 0) < 0) { rc = -errno; break; }
        }
    }
    closedir(dir);
    return rc < 0 ? rc : SyncDirectory(inodes_->fd);
}
}
