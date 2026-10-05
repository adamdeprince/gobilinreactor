#pragma once
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <fcntl.h>
#include <map>
#include <functional>

namespace goblin {

struct HostFile {
    explicit HostFile(int value = -1) : fd(value) {}
    ~HostFile();
    HostFile(const HostFile&) = delete;
    HostFile& operator=(const HostFile&) = delete;
    int fd;
};

// All guest paths are resolved from an open root directory. The walk follows
// guest symlinks itself; the host only ever sees individual O_NOFOLLOW names.
class Vfs {
public:
    bool Mount(const std::string& root, std::string* error);
    int Open(const std::string& path, int flags, unsigned mode = 0,
             std::string* canonical = nullptr) const;
    int Stat(const std::string& path, struct stat* out, bool follow = true) const;
    int StatFd(int fd, struct stat* out) const;
    long Readlink(const std::string& path, std::string* out) const;
    int Access(const std::string& path, int mode, bool follow = true) const;
    int AccessFd(int fd, int mode) const;
    int TimesFd(int fd, const struct timespec* times) const;
    int Mkdir(const std::string& path, unsigned mode) const;
    int Unlink(const std::string& path, bool directory) const;
    int Rename(const std::string& oldpath, const std::string& newpath) const;
    int Symlink(const std::string& target, const std::string& path) const;
    int Link(const std::string& oldpath, const std::string& newpath, bool follow) const;
    int Chmod(const std::string& path, unsigned mode) const;
    int ChmodFd(int fd, unsigned mode) const;
    int Chown(const std::string& path, unsigned uid, unsigned gid, bool follow) const;
    int ChownFd(int fd, unsigned uid, unsigned gid) const;
    int PathFd(int fd, std::string* path) const;
    int SetDiskLimit(uint64_t bytes);
    int StatFs(int fd, struct statfs* out) const;
    int CheckGrowth(int fd, uint64_t size) const;
    int CheckCreate(unsigned count = 1) const;
    void Track(const std::shared_ptr<HostFile>& file) const;
    uint64_t disk_usage() const { return disk_used_; }
    int Sync() const;
    // Fault injection runs only in the broker-side recovery tests.
    std::function<void(const char*)> transaction_hook;
    int Times(const std::string& path, const struct timespec* times, bool follow) const;
    long Lock(int fd, int owner, int command, struct flock* value);
    void ReleaseLocks(int owner, int fd = -1);
    bool ReadFile(const std::string& path, std::vector<uint8_t>* bytes,
                  std::string* error) const;
    bool mounted() const { return root_ != nullptr; }
    // A real path walk detects aliases into the virtual mount. It never
    // interprets guest /proc as the broker's procfs.
    int VirtualPath(const std::string& path, bool follow, std::string* out) const;
    bool proc_enabled = false;
private:
    struct Leaf {
        std::shared_ptr<HostFile> parent;
        std::string name, canonical;
        std::shared_ptr<HostFile> entry_parent;
        std::string entry_name;
        bool indirect = false;
        Leaf() = default;
        Leaf(std::shared_ptr<HostFile> p, std::string n, std::string c,
             std::shared_ptr<HostFile> ep = {}, std::string en = {}, bool i = false)
            : parent(std::move(p)), name(std::move(n)), canonical(std::move(c)),
              entry_parent(std::move(ep)), entry_name(std::move(en)), indirect(i) {}
    };
    int Resolve(const std::string& path, bool follow, bool missing, Leaf* out, bool detect_proc = false) const;
    int ModifyEntry(const Leaf& leaf, bool sticky) const;
    int NewMetadata(int fd, int parent, unsigned mode, bool directory = false) const;
    std::shared_ptr<HostFile> root_;
    mutable std::shared_ptr<HostFile> inodes_;
    mutable std::map<std::pair<dev_t, ino_t>, uint64_t> links_;
    int InodeStore() const;
    int LinkCount(const std::string& id, uint64_t count) const;
    int DropLink(const Leaf& leaf) const;
    void AdjustStat(struct stat* value) const;
    int Recover() const;
    int Promotion(const std::string& path, const std::string& id) const;
    int ClearPromotion() const;
    void Checkpoint(const char* name) const { if (transaction_hook) transaction_hook(name); }
    int ScanDisk() const;
    mutable uint64_t disk_used_ = 0;
    uint64_t disk_limit_ = UINT64_MAX;
    mutable std::map<std::pair<dev_t, ino_t>, uint64_t> disk_files_;
    mutable std::vector<std::weak_ptr<HostFile>> disk_open_;
    struct LockRange { dev_t device; ino_t inode; int owner; short type; int64_t start, end; };
    std::vector<LockRange> locks_;
};

// An APK fixture has a bounded binary format; extraction never follows links.
bool InstallRootfs(const uint8_t* data, size_t size, const std::string& directory,
                   std::string* error);

}  // namespace goblin
