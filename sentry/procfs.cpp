#include "files.h"
#include "sentry.h"
#include "guest_layout.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <iomanip>
#include <sstream>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

namespace goblin {
namespace {
bool IsProc(const std::string& path) { return path == "/proc" || path.compare(0, 6, "/proc/") == 0; }
uint64_t Inode(const std::string& path) {
    uint64_t n = 1469598103934665603ull;
    for (unsigned char c : path) n = (n ^ c) * 1099511628211ull;
    return n ? n : 1;
}
uint64_t Now() { timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t); return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec; }
std::vector<std::string> Parts(const std::string& path) {
    std::vector<std::string> result;
    for (size_t at = 0; at < path.size();) {
        auto end = path.find('/', at); if (end == std::string::npos) end = path.size();
        if (end > at) result.push_back(path.substr(at, end - at));
        at = end + 1;
    }
    return result;
}
bool Number(const std::string& s, int* value) {
    if (s.empty() || (s.size() > 1 && s[0] == '0')) return false;
    uint64_t n = 0;
    for (char c : s) { if (c < '0' || c > '9' || n > INT_MAX / 10) return false; n = n * 10 + c - '0'; }
    if (n > INT_MAX) return false;
    *value = n; return true;
}
std::string Parent(const std::string& path) { size_t at = path.rfind('/'); return at == 0 ? "/" : path.substr(0, at); }
struct Node {
    unsigned mode = S_IFREG | 0444;
    uint32_t uid = 0, gid = 0;
    std::string data, link;
    std::vector<ProcHandle::Entry> entries;
    std::shared_ptr<OpenFile> file;
};
void Entry(Node& node, const std::string& path, const std::string& name, unsigned char type) {
    node.entries.push_back({name, type, Inode(path + "/" + name)});
}
std::string FileLink(const OpenFile& file) {
    if (!file.path.empty()) return file.path;
    if (file.proc) return file.proc->path;
    if (file.socket_domain) return "socket:[" + std::to_string(file.identity) + "]";
    if (file.pollable || file.kind != OpenFile::kHost) return "pipe:[" + std::to_string(file.identity) + "]";
    return "anon_inode:[goblin]";
}
int Lookup(const FileTable& table, const std::string& path, Node* node, bool data = false) {
    auto parts = Parts(path);
    if (parts.empty() || parts[0] != "proc") return -ENOENT;
    auto& state = *table.proc;
    if (parts.size() == 1) {
        node->mode = S_IFDIR | 0555;
        for (const char* name : {"self", "thread-self"}) Entry(*node, path, name, DT_LNK);
        for (const char* name : {"stat", "uptime", "meminfo", "cpuinfo", "version", "mounts"}) Entry(*node, path, name, DT_REG);
        Entry(*node, path, "sys", DT_DIR);
        for (auto [tid, tgid] : state.tasks()) if (tid == tgid) Entry(*node, path, std::to_string(tgid), DT_DIR);
        return 0;
    }
    if (parts[1] == "self" || parts[1] == "thread-self") {
        if (parts.size() != 2) return -ENOENT;
        node->mode = S_IFLNK | 0777;
        node->link = std::to_string(table.lock_owner);
        if (parts[1] == "thread-self") node->link += "/task/" + std::to_string(table.current_tid);
        return 0;
    }
    const double elapsed = double(Now() - state.started_ns) / 1e9;
    if (parts.size() == 2) {
        if (parts[1] == "uptime") { std::ostringstream s; s << std::fixed << std::setprecision(2) << elapsed << " 0.00\n"; node->data = s.str(); return 0; }
        if (parts[1] == "stat") {
            node->data = "cpu 0 0 0 0 0 0 0 0 0 0\ncpu0 0 0 0 0 0 0 0 0 0 0\nintr 0\nctxt 0\nbtime " +
                std::to_string(time(nullptr) - uint64_t(elapsed)) + "\nprocesses " + std::to_string(state.tasks().size()) + "\nprocs_running 1\nprocs_blocked 0\n";
            return 0;
        }
        if (parts[1] == "meminfo") {
            node->data = "MemTotal: " + std::to_string(state.memory_limit / 1024) + " kB\nMemFree: 0 kB\nMemAvailable: 0 kB\nBuffers: 0 kB\nCached: 0 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n";
            return 0;
        }
        if (parts[1] == "cpuinfo") { node->data = "processor\t: 0\nBogoMIPS\t: 0.00\nFeatures\t: fp asimd\nCPU architecture: 8\n"; return 0; }
        if (parts[1] == "version") { node->data = "Linux version 6.6.0-goblin (userspace guest kernel)\n"; return 0; }
        if (parts[1] == "mounts") { node->data = "goblin / goblin rw 0 0\nproc /proc proc ro 0 0\n"; return 0; }
    }
    if (parts[1] == "sys") {
        if (parts.size() == 2 || (parts.size() == 3 && parts[2] == "kernel")) {
            node->mode = S_IFDIR | 0555;
            if (parts.size() == 2) Entry(*node, path, "kernel", DT_DIR);
            else for (const char* name : {"pid_max", "osrelease", "ostype"}) Entry(*node, path, name, DT_REG);
            return 0;
        }
        if (parts.size() == 4 && parts[2] == "kernel") {
            if (parts[3] == "pid_max") node->data = "2147483647\n";
            else if (parts[3] == "osrelease") node->data = "6.6.0-goblin\n";
            else if (parts[3] == "ostype") node->data = "Linux\n";
            else return -ENOENT;
            return 0;
        }
        return -ENOENT;
    }
    int pid;
    if (!Number(parts[1], &pid) || pid <= 0) return -ENOENT;
    ProcTask task;
    if (!state.task(pid, ProcState::kBasic, &task)) return -ENOENT;
    size_t at = 2;
    if (parts.size() > at && parts[at] == "task") {
        if (parts.size() == at + 1) {
            node->mode = S_IFDIR | 0555;
            for (auto [tid, tgid] : state.tasks()) if (tgid == task.tgid) Entry(*node, path, std::to_string(tid), DT_DIR);
            return 0;
        }
        int tid; ProcTask thread;
        if (!Number(parts[at + 1], &tid) || !state.task(tid, 0, &thread) || thread.tgid != task.tgid) return -ENOENT;
        pid = tid; task = std::move(thread); at += 2;
    }
    node->uid = task.credentials.euid; node->gid = task.credentials.egid;
    if (parts.size() == at) {
        node->mode = S_IFDIR | 0555;
        for (const char* name : {"status", "stat", "statm", "cmdline", "environ", "comm", "maps", "auxv", "wchan", "cgroup"}) Entry(*node, path, name, DT_REG);
        for (const char* name : {"exe", "cwd", "root"}) Entry(*node, path, name, DT_LNK);
        for (const char* name : {"fd", "task"}) Entry(*node, path, name, DT_DIR);
        return 0;
    }
    node->uid = task.credentials.euid; node->gid = task.credentials.egid;
    const auto& item = parts[at];
    const auto& c = CurrentCredentials();
    if (item == "fd" || item == "maps" || item == "environ" || item == "auxv" || item == "exe" || item == "cwd" || item == "root") {
        if (c.fsuid && (c.fsuid != task.credentials.uid || c.fsuid != task.credentials.euid || c.fsuid != task.credentials.suid)) return -EACCES;
    }
    if (item == "fd") {
        if (!state.task(pid, ProcState::kFiles, &task)) return -ENOENT;
        if (parts.size() == at + 1) {
            node->mode = S_IFDIR | 0500;
            for (const auto& [fd, file] : task.files) Entry(*node, path, std::to_string(fd), DT_LNK);
            return 0;
        }
        int fd;
        if (parts.size() != at + 2 || !Number(parts[at + 1], &fd)) return -ENOENT;
        for (const auto& entry : task.files) if (entry.first == fd) {
            node->mode = S_IFLNK | 0700; node->file = entry.second; node->link = FileLink(*entry.second); return 0;
        }
        return -ENOENT;
    }
    if (parts.size() != at + 1) return -ENOTDIR;
    if (item == "exe" || item == "cwd" || item == "root") {
        if (task.state == 'Z') return -ENOENT;
        node->mode = S_IFLNK | 0777;
        node->link = item == "exe" ? task.executable : item == "cwd" ? task.cwd : "/";
        return 0;
    }
    if (item == "status") {
        std::ostringstream s;
        s << "Name:\t" << task.name << "\nState:\t" << task.state << "\nTgid:\t" << task.tgid << "\nPid:\t" << task.pid
          << "\nPPid:\t" << task.ppid << "\nTracerPid:\t0\nUid:\t"
          << task.credentials.uid << '\t' << task.credentials.euid << '\t' << task.credentials.suid << '\t' << task.credentials.fsuid
          << "\nGid:\t" << task.credentials.gid << '\t' << task.credentials.egid << '\t' << task.credentials.sgid << '\t' << task.credentials.fsgid
          << "\nFDSize:\t256\nGroups:\t";
        for (auto group : task.credentials.groups) s << group << ' ';
        s << "\nVmSize:\t" << task.bytes / 1024 << " kB\nVmRSS:\t0 kB\nThreads:\t" << task.threads << '\n'
          << "SigQ:\t0/0\nSigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\nSigBlk:\t"
          << std::hex << std::setw(16) << std::setfill('0') << task.blocked_signals
          << "\nSigIgn:\t0000000000000000\nSigCgt:\t0000000000000000\nCapInh:\t0000000000000000\nCapPrm:\t0000000000000000\nCapEff:\t0000000000000000\n";
        node->data = s.str(); return 0;
    }
    if (item == "stat") {
        // Fields not yet accounted (CPU ticks and resident pages) are zero.
        std::ostringstream s; s << task.pid << " (" << task.name << ") " << task.state;
        for (int field = 4; field <= 52; ++field) {
            uint64_t value = 0;
            switch (field) {
                case 4: value = task.ppid; break; case 5: value = task.group; break; case 6: value = task.session; break;
                case 7: value = task.tty; break; case 8: value = task.foreground; break;
                case 18: value = 20; break; case 20: value = task.threads; break; case 22: value = task.started; break;
                case 23: value = task.bytes; break; case 25: value = INT64_MAX; break;
            }
            s << ' ' << value;
        }
        node->data = s.str() + "\n"; return 0;
    }
    if (item == "statm") { node->data = std::to_string(task.bytes / sysconf(_SC_PAGESIZE)) + " 0 0 0 0 0 0\n"; return 0; }
    if (item == "comm") { node->data = task.name + "\n"; return 0; }
    if (item == "maps") {
        if (data && !state.task(pid, ProcState::kMaps, &task)) return -ENOENT;
        node->data = std::move(task.maps); return node->data.size() > (8u << 20) ? -EFBIG : 0;
    }
    if (item == "cmdline" || item == "environ") {
        if (data && !state.task(pid, ProcState::kArguments, &task)) return -ENOENT;
        node->data = item == "cmdline" ? std::move(task.command) : std::move(task.environment); return 0;
    }
    if (item == "auxv") {
        uint64_t aux[] = {6, uint64_t(sysconf(_SC_PAGESIZE)), 17, 100, 11, task.credentials.uid, 12, task.credentials.euid, 13, task.credentials.gid, 14, task.credentials.egid, 23, 0, 0, 0};
        node->data.assign(reinterpret_cast<char*>(aux), sizeof(aux)); return 0;
    }
    if (item == "wchan") { node->data = "0\n"; return 0; }
    if (item == "cgroup") { node->data = "0::/\n"; return 0; }
    return -ENOENT;
}
void Stat(const std::string& path, unsigned mode, struct stat* st, uint64_t size = 0) {
    *st = {}; st->st_dev = 0; st->st_ino = Inode(path); st->st_mode = mode;
    st->st_nlink = S_ISDIR(mode) ? 2 : 1; st->st_blksize = 4096; st->st_size = size;
}
}

int FileTable::ResolveSpecial(std::string* path, bool follow) const {
    if (!proc->task || !vfs->proc_enabled) return 0;
    unsigned links = 0;
    for (unsigned walks = 0; walks < 256; ++walks) {
        std::string virtual_path;
        if (!vfs->VirtualPath(*path, follow, &virtual_path)) return 0;
        auto pieces = Parts(virtual_path);
        std::deque<std::string> pending(pieces.begin() + 1, pieces.end());
        std::string current = "/proc";
        bool restart = false;
        while (!pending.empty()) {
            std::string part = pending.front(); pending.pop_front();
            if (part == ".") continue;
            if (part == "..") {
                current = Parent(current);
                if (!IsProc(current)) { restart = true; break; }
                continue;
            }
            current += "/" + part;
            Node node; int rc = Lookup(*this, current, &node); if (rc < 0) return rc;
            if (S_ISLNK(node.mode) && (follow || !pending.empty())) {
                if (++links > 40) return -ELOOP;
                if (node.file) {
                    if (pending.empty()) { *path = current; return 0; }
                    if (!node.file->directory) return -ENOTDIR;
                    if (node.file->proc) current = node.file->proc->path;
                    else { rc = vfs->PathFd(node.file->host->fd, &current); if (rc < 0) return rc; }
                } else current = node.link[0] == '/' ? node.link : Parent(current) + "/" + node.link;
                restart = true; break;
            }
            if (!pending.empty() && !S_ISDIR(node.mode)) return -ENOTDIR;
        }
        for (const auto& part : pending) current += "/" + part;
        *path = current;
        if (path->size() > 4096) return -ENAMETOOLONG;
        if (!restart) return 0;
    }
    return -ELOOP;
}

std::optional<long> FileTable::Proc(const SyscallRequest& req, const GuestMemory& mem) {
    if (!proc->task) return {};
    const auto& a = req.args;
    const auto* descriptor = Get(a[0]);
    auto handle = descriptor ? descriptor->file->proc : nullptr;
    auto stat_file = [&](const std::shared_ptr<OpenFile>& file, struct stat* st) {
        if (file->proc) { Stat(file->proc->path, file->proc->mode, st); st->st_uid = file->proc->uid; st->st_gid = file->proc->gid; return 0; }
        if (file->host) return vfs->StatFd(file->host->fd, st);
        *st = {}; st->st_mode = (file->socket_domain ? S_IFSOCK : S_IFIFO) | 0600; st->st_ino = file->identity; st->st_nlink = 1; st->st_blksize = 4096; return 0;
    };
    if (handle && req.nr == 80) {
        struct stat st{}; Stat(handle->path, handle->mode, &st); st.st_uid = handle->uid; st.st_gid = handle->gid;
        return mem.Write(a[1], &st, sizeof(st)) ? 0 : -EFAULT;
    }
    if (handle && req.nr == 61) {
        if (!descriptor->file->directory) return -ENOTDIR;
        if (descriptor->file->flags & O_PATH) return -EBADF;
        std::vector<uint8_t> bytes; size_t index = handle->offset, capacity = std::min<size_t>(a[2], 1 << 20);
        while (index < handle->entries.size()) {
            const auto& entry = handle->entries[index];
            uint16_t size = (19 + entry.name.size() + 1 + 7) & ~size_t(7);
            if (size > capacity - bytes.size()) { if (bytes.empty()) return -EINVAL; break; }
            size_t at = bytes.size(); bytes.resize(at + size, 0);
            uint64_t next = index + 1;
            memcpy(bytes.data() + at, &entry.inode, 8); memcpy(bytes.data() + at + 8, &next, 8);
            memcpy(bytes.data() + at + 16, &size, 2); bytes[at + 18] = entry.type;
            memcpy(bytes.data() + at + 19, entry.name.c_str(), entry.name.size()); ++index;
        }
        if (!mem.Write(a[1], bytes.data(), bytes.size())) return -EFAULT;
        handle->offset = index; return long(bytes.size());
    }
    if (handle && req.nr == 62) {
        if (descriptor->file->flags & O_PATH) return -EBADF;
        if (a[2] > SEEK_END) return -EINVAL;
        int64_t base = a[2] == SEEK_SET ? 0 : a[2] == SEEK_CUR ? handle->offset : handle->data.size();
        int64_t offset = a[1];
        if ((offset > 0 && base > INT64_MAX - offset) || (offset < 0 && (offset == INT64_MIN || base < -offset))) return -EINVAL;
        handle->offset = base + offset; return handle->offset;
    }
    if (handle && req.nr == 50) {
        if (!descriptor->file->directory) return -ENOTDIR;
        cwd = handle->path; cwd_handle.reset(); return 0;
    }
    if (handle && (req.nr == 46 || req.nr == 52 || req.nr == 55 || req.nr == 82 || req.nr == 83)) return -EROFS;
    std::string path; int rc = 0; bool path_request = true, follow = true;
    switch (req.nr) {
        case 17: return {}; // getcwd already understands a virtual cwd.
        case 34: case 35: follow = false; [[fallthrough]];
        case 48: case 53: case 439: rc = Path(a[0], a[1], mem, &path, follow); break;
        case 56: rc = Path(a[0], a[1], mem, &path, !(a[2] & O_NOFOLLOW) && !((a[2] & O_CREAT) && (a[2] & O_EXCL))); break;
        case 43: case 45: case 49: rc = Path(AT_FDCWD, a[0], mem, &path); break;
        case 54: rc = Path(a[0], a[1], mem, &path, !(a[4] & AT_SYMLINK_NOFOLLOW)); break;
        case 88: if (!a[1]) return {}; rc = Path(a[0], a[1], mem, &path, !(a[3] & AT_SYMLINK_NOFOLLOW)); break;
        case 78: rc = Path(a[0], a[1], mem, &path, false); break;
        case 79: {
            std::string raw; if (!mem.ReadString(a[1], &raw)) return -EFAULT;
            if (raw.empty() && (a[3] & AT_EMPTY_PATH) && handle) {
                struct stat st{}; Stat(handle->path, handle->mode, &st); st.st_uid = handle->uid; st.st_gid = handle->gid;
                return mem.Write(a[2], &st, sizeof(st)) ? 0 : -EFAULT;
            }
            if (raw.empty()) return {};
            rc = Path(a[0], a[1], mem, &path, !(a[3] & AT_SYMLINK_NOFOLLOW)); break;
        }
        case 36: rc = Path(a[1], a[2], mem, &path, false); break;
        case 37: case 38: case 276: {
            std::string other;
            rc = Path(a[0], a[1], mem, &path, req.nr == 37 && (a[4] & AT_SYMLINK_FOLLOW));
            if (rc < 0) return rc;
            rc = Path(a[2], a[3], mem, &other, false);
            if (rc < 0) return rc;
            return IsProc(path) || IsProc(other) ? std::optional<long>(-EROFS) : std::nullopt;
        }
        default: path_request = false;
    }
    if (!path_request && !(handle && req.nr == 44)) return {};
    if (rc < 0) return rc;
    if (path_request && !IsProc(path)) return {};
    if (req.nr == 43 || req.nr == 44) {
        struct statfs st{}; st.f_type = 0x9fa0; st.f_bsize = 4096; st.f_namelen = 255;
        return mem.Write(a[1], &st, sizeof(st)) ? 0 : -EFAULT;
    }
    Node node; rc = Lookup(*this, path, &node, req.nr == 56); if (rc < 0) return rc;
    if (req.nr == 53 && node.file && node.file->host) {
        // glibc implements fchmodat(AT_SYMLINK_NOFOLLOW) through an O_PATH
        // descriptor and /proc/self/fd. Change the pinned guest inode, not
        // the procfs link, and retain the normal guest ownership checks.
        struct stat st{}; rc = stat_file(node.file, &st); if (rc < 0) return rc;
        if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) return vfs->ChmodFd(node.file->host->fd, a[2]);
    }
    if (req.nr == 78) {
        if (!a[3]) return -EINVAL;
        if (!S_ISLNK(node.mode)) return -EINVAL;
        size_t size = std::min<size_t>(a[3], node.link.size());
        return mem.Write(a[2], node.link.data(), size) ? long(size) : -EFAULT;
    }
    if (req.nr == 79) {
        if (a[3] & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT)) return -EINVAL;
        struct stat st{};
        if (node.file && !(a[3] & AT_SYMLINK_NOFOLLOW)) rc = stat_file(node.file, &st);
        else { Stat(path, node.mode, &st, S_ISLNK(node.mode) ? node.link.size() : 0); st.st_uid = node.uid; st.st_gid = node.gid; }
        if (rc < 0) return rc;
        return mem.Write(a[2], &st, sizeof(st)) ? 0 : -EFAULT;
    }
    if (req.nr == 48 || req.nr == 439) {
        if (a[2] & ~(R_OK | W_OK | X_OK)) return -EINVAL;
        if (a[2] & W_OK) return -EROFS;
        return (a[2] & X_OK) && !S_ISDIR(node.mode) ? -EACCES : 0;
    }
    if (req.nr == 49) {
        if (!S_ISDIR(node.mode)) return -ENOTDIR;
        cwd = path; cwd_handle.reset(); return 0;
    }
    if (req.nr != 56) return -EROFS;
    const int flags = a[2];
    const int allowed = O_ACCMODE | O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC | O_APPEND | O_NONBLOCK | O_DSYNC | O_SYNC | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_PATH | O_LARGEFILE;
    if (flags & ~allowed) return -EINVAL;
    if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
    if (S_ISLNK(node.mode) && (flags & O_NOFOLLOW) && !(flags & O_PATH)) return -ELOOP;
    if (node.file && !(flags & O_NOFOLLOW)) {
        if (node.file->socket_domain) return -ENXIO;
        if ((flags & O_DIRECTORY) && !node.file->directory) return -ENOTDIR;
        auto file = std::make_shared<OpenFile>(*node.file);
        if (file->host) {
            if (!(flags & O_PATH)) {
                int access = (flags & O_ACCMODE) == O_RDONLY ? R_OK : (flags & O_ACCMODE) == O_WRONLY ? W_OK : R_OK | W_OK;
                if (flags & O_TRUNC) access |= W_OK;
                rc = vfs->AccessFd(file->host->fd,access); if (rc < 0) return rc;
                if (file->terminal && CurrentCredentials().fsuid && CurrentCredentials().fsuid != file->terminal->uid) return -EACCES;
            }
            int fd = open(("/proc/self/fd/" + std::to_string(file->host->fd)).c_str(), (flags & ~O_CLOEXEC) | O_CLOEXEC | O_NONBLOCK);
            if (fd < 0) return -errno;
            file->host = std::make_shared<HostFile>(fd); vfs->Track(file->host);
        } else if (file->proc) {
            if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC)) return -EACCES;
            file->proc = std::make_shared<ProcHandle>(*file->proc); file->proc->offset = 0;
        }
        file->flags = flags & ~(O_CLOEXEC | O_CREAT | O_EXCL | O_TRUNC);
        return Install(file, flags & O_CLOEXEC);
    }
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) return -EACCES;
    if ((flags & O_DIRECTORY) && !S_ISDIR(node.mode)) return -ENOTDIR;
    auto file = std::make_shared<OpenFile>(); file->path = path;
    file->flags = flags & ~O_CLOEXEC; file->directory = S_ISDIR(node.mode);
    file->proc = std::make_shared<ProcHandle>();
    auto& p = *file->proc; p.path = path; p.data = std::move(node.data); p.mode = node.mode; p.uid = node.uid; p.gid = node.gid; p.inode = Inode(path);
    if (file->directory) {
        p.entries = {{".", DT_DIR, p.inode}, {"..", DT_DIR, Inode(Parent(path))}};
        p.entries.insert(p.entries.end(), node.entries.begin(), node.entries.end());
    }
    return Install(file, flags & O_CLOEXEC);
}
}
