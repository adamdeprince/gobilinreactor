#include "archive.h"
#include "protocol.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <vector>

namespace goblin_uml {
namespace {
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) { if (fd < 0) throw std::runtime_error(strerror(errno)); }
    ~Fd() { close(value); }
    Fd(const Fd&) = delete;
};
void Check(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what + ": " + strerror(errno));
}
void Write(int fd, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        Check(n > 0, "writing migration archive"); p += n; size -= n;
    }
}
std::string Link(int directory, const char* name) {
    std::vector<char> bytes(256);
    for (;;) {
        ssize_t n = readlinkat(directory, name, bytes.data(), bytes.size());
        Check(n >= 0, "reading link");
        if (size_t(n) < bytes.size()) return {bytes.data(), size_t(n)};
        bytes.resize(bytes.size() * 2);
    }
}
}
bool ExportLegacy(const std::string& root, const std::string& output, std::string* error,
                  const std::vector<std::string>& omit) {
    const std::string temporary = output + ".new";
    try {
        Fd base(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        // Complete the only legacy transaction that can leave a directory entry
        // temporarily absent. Do not alter the old filesystem to recover it.
        std::map<std::string, std::string> pending;
        int journal = openat(base.value, ".goblin-inodes/promotion", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (journal >= 0) {
            Fd owner(journal); std::string bytes; char chunk[4096]; ssize_t n;
            while ((n = read(journal, chunk, sizeof(chunk))) > 0) bytes.append(chunk, n);
            Check(n == 0, "reading migration journal");
            size_t split = bytes.find('\0');
            Check(split != std::string::npos && bytes.back() == '\0', "invalid migration journal");
            std::string id = bytes.substr(0, split), path = bytes.substr(split + 1, bytes.size() - split - 2);
            Check(!id.empty() && id.find_first_not_of("0123456789-") == std::string::npos && !path.empty() && path.front() == '/' &&
                  path.find('\0') == std::string::npos && (path + "/").find("/../") == std::string::npos, "invalid promotion path");
            struct stat old{}, stored{};
            if (fstatat(base.value, path.c_str() + 1, &old, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT &&
                fstatat(base.value, (".goblin-inodes/" + id).c_str(), &stored, AT_SYMLINK_NOFOLLOW) == 0)
                pending[path.substr(1)] = ".goblin-inodes/" + id;
        } else Check(errno == ENOENT, "opening migration journal");
        Fd out(open(temporary.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600));
        Write(out.value, "GOBLINU2", 8);
        std::map<std::pair<dev_t, ino_t>, std::string> inodes;
        std::function<void(int, const std::string&, const std::string&)> visit;
        visit = [&](int parent, const std::string& leaf, const std::string& name) {
            if (std::find(omit.begin(), omit.end(), name) != omit.end()) return;
            struct stat st{}; Check(fstatat(parent, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0, name);
            std::string target, source = leaf;
            int directory = parent;
            if (S_ISLNK(st.st_mode)) {
                target = Link(parent, leaf.c_str());
                if (target.rfind("goblin-inode:", 0) == 0) {
                    std::string id = target.substr(13);
                    Check(!id.empty() && id.find_first_not_of("0123456789-") == std::string::npos, "invalid legacy inode link");
                    directory = base.value; source = ".goblin-inodes/" + id; target.clear();
                    Check(fstatat(directory, source.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0, source);
                    if (S_ISLNK(st.st_mode)) target = Link(directory, source.c_str());
                }
            }
            Entry entry{uint32_t(name.size()), uint32_t(st.st_mode), 0, 0, 0, st.st_mtim.tv_sec, uint32_t(st.st_mtim.tv_nsec), 0};
            int descriptor = -1;
            struct CloseDescriptor { int& fd; ~CloseDescriptor() { if (fd >= 0) close(fd); } } close_descriptor{descriptor};
            if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) {
                descriptor = openat(directory, source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                Check(descriptor >= 0, name);
                struct Metadata { uint32_t magic, mode, uid, gid; } metadata{};
                ssize_t n = fgetxattr(descriptor, "user.goblin.metadata.v1", &metadata, sizeof(metadata));
                if (n >= 0) {
                    Check(n == sizeof(metadata) && metadata.magic == 0x474d4431, "invalid ownership metadata: " + name);
                    entry.mode = (st.st_mode & S_IFMT) | (metadata.mode & 07777); entry.uid = metadata.uid; entry.gid = metadata.gid;
                } else Check(errno == ENODATA, "reading ownership metadata");
                n = fgetxattr(descriptor, "user.goblin.symlink.v1", nullptr, 0);
                if (n >= 0) {
                    target.resize(n);
                    Check(n > 0 && fgetxattr(descriptor, "user.goblin.symlink.v1", target.data(), n) == n, "reading symbolic link metadata");
                    entry.mode = S_IFLNK | 0777;
                } else Check(errno == ENODATA, "reading link metadata");
                uint32_t socket;
                n = fgetxattr(descriptor, "user.goblin.socket.v1", &socket, sizeof(socket));
                if (n >= 0) { Check(n == 4 && socket == 0x47534f31, "invalid socket metadata"); entry.mode = S_IFSOCK | (entry.mode & 07777); }
                else Check(errno == ENODATA, "reading socket metadata");
            }
            const auto key = std::make_pair(st.st_dev, st.st_ino);
            if (!S_ISDIR(entry.mode) && inodes.count(key)) { entry.flags = HardLink; target = inodes.at(key); }
            else if (!S_ISDIR(entry.mode)) inodes[key] = name;
            entry.size = entry.flags == HardLink || S_ISLNK(entry.mode) ? target.size() : S_ISREG(entry.mode) ? uint64_t(st.st_size) : 0;
            Write(out.value, &entry, sizeof(entry)); Write(out.value, name.data(), name.size());
            if (entry.flags == HardLink || S_ISLNK(entry.mode)) Write(out.value, target.data(), target.size());
            else if (S_ISREG(entry.mode)) {
                uint64_t remaining = entry.size; char bytes[65536];
                while (remaining) {
                    ssize_t n = read(descriptor, bytes, std::min<uint64_t>(remaining, sizeof(bytes)));
                    if (n < 0 && errno == EINTR) continue;
                    Check(n > 0, "reading " + name);
                    // Keep sparse source files sparse in the temporary archive.
                    if (std::all_of(bytes, bytes + n, [](char c) { return c == 0; })) Check(lseek(out.value, n, SEEK_CUR) >= 0, "seeking archive");
                    else Write(out.value, bytes, n);
                    remaining -= n;
                }
            }
            if (S_ISDIR(entry.mode)) {
                DIR* dir = fdopendir(descriptor); Check(dir != nullptr, name); descriptor = -1;
                std::unique_ptr<DIR, decltype(&closedir)> close_directory(dir, closedir);
                std::vector<std::string> children;
                while (auto* child = readdir(dir)) {
                    std::string value = child->d_name;
                    if (value != "." && value != ".." && value != ".goblin-inodes") children.push_back(value);
                }
                std::sort(children.begin(), children.end());
                for (const auto& child : children) visit(dirfd(dir), child, name.empty() ? child : name + "/" + child);
                for (const auto& item : pending) {
                    size_t slash = item.first.rfind('/');
                    if ((slash == std::string::npos ? std::string() : item.first.substr(0, slash)) == name)
                        visit(base.value, item.second, item.first);
                }
                entry.flags = MetadataOnly; Write(out.value, &entry, sizeof(entry)); Write(out.value, name.data(), name.size());
            }
        };
        visit(base.value, ".", "");
        Entry end{}; end.name_size = UINT32_MAX; Write(out.value, &end, sizeof(end));
        Check(fsync(out.value) == 0, "syncing migration archive");
        Check(rename(temporary.c_str(), output.c_str()) == 0, "publishing migration archive");
        return true;
    } catch (const std::exception& exception) {
        *error = exception.what(); unlink(temporary.c_str()); return false;
    }
}
}
