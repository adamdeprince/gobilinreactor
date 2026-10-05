#include "files.h"
#include "vfs_metadata.h"
#include <atomic>
#include <sys/socket.h>
#include "sentry.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <termios.h>
#include <unistd.h>

namespace goblin {
namespace {
long Result(long value) { return value < 0 ? -errno : value; }
constexpr size_t kIoLimit = 1 << 20;
struct KernelTermios { uint32_t input, output, control, local; uint8_t line, cc[19]; };
static_assert(sizeof(KernelTermios) == 36);
}
FileTable::FileTable() : vfs(std::make_shared<Vfs>()), output(std::make_shared<Output>()),
    terminals_(std::make_shared<std::map<int, std::weak_ptr<Terminal>>>()) {
    for (int fd = 0; fd < 3; ++fd) {
        auto file = std::make_shared<OpenFile>();
        file->kind = fd == 0 ? OpenFile::kInput : fd == 1 ? OpenFile::kOutput : OpenFile::kError;
        file->flags = fd == 0 ? O_RDONLY : O_WRONLY;
        descriptors_.emplace(fd, Descriptor{file, false});
    }
}
namespace {
void TerminalMode(const Terminal& terminal, struct stat* st) {
    st->st_uid=terminal.uid; st->st_gid=terminal.gid; st->st_mode=S_IFCHR | terminal.mode;
}
int TerminalStat(const Terminal& terminal, struct stat* st) {
    if (stat(terminal.host_slave.c_str(),st)<0) return -errno;
    TerminalMode(terminal,st);
    return 0;
}
int TerminalChmod(Terminal& terminal, unsigned mode) {
    const auto& c=CurrentCredentials();
    if (c.fsuid && c.fsuid!=terminal.uid) return -EPERM;
    terminal.mode=mode & 0777; return 0;
}
int TerminalChown(Terminal& terminal, unsigned uid, unsigned gid) {
    const auto& c=CurrentCredentials();
    if (c.fsuid && (c.fsuid!=terminal.uid || (uid!=UINT32_MAX && uid!=terminal.uid) ||
        (gid!=UINT32_MAX && gid!=terminal.gid && !c.InGroup(gid)))) return -EPERM;
    if (uid!=UINT32_MAX) terminal.uid=uid;
    if (gid!=UINT32_MAX) terminal.gid=gid;
    return 0;
}
}
std::shared_ptr<Terminal> FileTable::TerminalAt(const std::string& path) const {
    auto terminal=controlling_terminal();
    if (path!="/dev/tty") {
        if (path.compare(0,9,"/dev/pts/")!=0) return {};
        const std::string number=path.substr(9);
        if (number.empty() || number.size()>2 || number.find_first_not_of("0123456789")!=std::string::npos) return {};
        auto found=terminals_->find(atoi(number.c_str()));
        terminal=found==terminals_->end()?nullptr:found->second.lock();
    }
    return terminal && !terminal->master.expired()?terminal:nullptr;
}
std::shared_ptr<OpenFile> FileTable::NewTerminal(int* error) {
    int number = 0;
    while (number < 16 && !(*terminals_)[number].expired()) ++number;
    if (number == 16) { *error = -ENOSPC; return {}; }
    int fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) { *error = -errno; return {}; }
    auto file = std::make_shared<OpenFile>();
    file->host = std::make_shared<HostFile>(fd);
    char path[128];
    int rc = ptsname_r(fd, path, sizeof(path));
    if (rc) { *error = -rc; return {}; }
    if (grantpt(fd) < 0) { *error = -errno; return {}; }
    file->flags = O_RDWR; file->pollable = true; file->pty_master = true;
    file->path = "/dev/ptmx"; file->terminal = std::make_shared<Terminal>();
    file->terminal->master = file->host; file->terminal->host_slave = path;
    file->terminal->number = number;
    file->terminal->uid = CurrentCredentials().fsuid; file->terminal->gid = CurrentCredentials().fsgid;
    // A newly allocated PTY has no controlling session until it is acquired.
    (*terminals_)[number] = file->terminal;
    return file;
}
void FileTable::ShareNamespace(const FileTable& other) {
    vfs = other.vfs; dns = other.dns; proc = other.proc;
    unix_namespace = other.unix_namespace; terminals_ = other.terminals_;
}
bool FileTable::UseTerminal(const std::string& input, std::string* error) {
    int rc = 0;
    auto master = NewTerminal(&rc);
    if (!master) { *error = std::string("creating PTY: ") + strerror(-rc); return false; }
    if (unlockpt(master->host->fd) < 0) { *error = "unlocking PTY"; return false; }
    master->terminal->locked = false;
    master->terminal->session = session;
    master->terminal->foreground_group = foreground_group;
    int fd = open(master->terminal->host_slave.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) { *error = std::string("opening PTY slave: ") + strerror(errno); return false; }
    auto slave = std::make_shared<OpenFile>();
    slave->host = std::make_shared<HostFile>(fd); slave->flags = O_RDWR;
    slave->pollable = true; slave->terminal = master->terminal;
    slave->path = "/dev/pts/" + std::to_string(slave->terminal->number);
    winsize size{24, 80, 0, 0}; ioctl(fd, TIOCSWINSZ, &size);
    for (int i = 0; i < 3; ++i) descriptors_[i] = {slave, false};
    controlling_terminal_ = slave->terminal;
    terminal_transport = master->host;
    terminal_input = input;
    return true;
}
const Descriptor* FileTable::Get(int fd) const {
    auto it = descriptors_.find(fd);
    return it == descriptors_.end() ? nullptr : &it->second;
}
int FileTable::HostFd(int fd) const {
    const auto* d = Get(fd);
    return d && d->file->host ? d->file->host->fd : -1;
}
int FileTable::Install(std::shared_ptr<OpenFile> file, bool cloexec, int minimum) {
    static std::atomic<uint64_t> next_identity{1};
    if (!file->identity) file->identity = next_identity.fetch_add(1);
    if (minimum < 0 || minimum >= 256) return -EINVAL;
    for (int fd = minimum; fd < 256; ++fd) {
        if (!Get(fd)) { descriptors_[fd] = {std::move(file), cloexec}; return fd; }
    }
    return -EMFILE;
}
void FileTable::CloseExec() {
    unix_namespace->dirty = true;
    for (auto it = descriptors_.begin(); it != descriptors_.end();)
        if (it->second.cloexec) {
            if (it->second.file->host) vfs->ReleaseLocks(lock_owner, it->second.file->host->fd);
            it = descriptors_.erase(it);
        } else ++it;
}
void FileTable::CloseRange(unsigned first, unsigned last, bool cloexec) {
    unix_namespace->dirty = true;
    for (auto it = descriptors_.begin(); it != descriptors_.end();) {
        if (unsigned(it->first) < first || unsigned(it->first) > last) { ++it; continue; }
        if (cloexec) { it->second.cloexec = true; ++it; }
        else {
            if (it->second.file->host) vfs->ReleaseLocks(lock_owner, it->second.file->host->fd);
            it = descriptors_.erase(it);
        }
    }
}
int FileTable::Path(int dirfd, uintptr_t address, const GuestMemory& memory, std::string* path, bool follow) const {
    if (!memory.ReadString(address, path)) return -EFAULT;
    if (path->empty()) return -ENOENT;
    if ((*path)[0] == '/') return ResolveSpecial(path, follow);
    std::string base = cwd;
    if (dirfd == AT_FDCWD && cwd_handle) { int rc = vfs->PathFd(cwd_handle->fd, &base); if (rc < 0) return rc; }
    if (dirfd != AT_FDCWD) {
        const auto* descriptor = Get(dirfd);
        if (!descriptor) return -EBADF;
        if (!descriptor->file->directory) return -ENOTDIR;
        if (descriptor->file->proc) base = descriptor->file->proc->path;
        else { int rc = vfs->PathFd(descriptor->file->host->fd, &base); if (rc < 0) return rc; }
    }
    *path = base + "/" + *path;
    return path->size() > 4096 ? -ENAMETOOLONG : ResolveSpecial(path, follow);
}
long FileTable::Read(int fd, uintptr_t buffer, size_t size, const GuestMemory& memory,
                     bool positional, uint64_t offset) {
    auto* d = Get(fd);
    if (!d || (d->file->flags & O_ACCMODE) == O_WRONLY || (d->file->flags & O_PATH)) return -EBADF;
    if (d->file->directory) return -EISDIR;
    if (d->file->unix_socket) {
        if (positional) return -ESPIPE;
        SyscallRequest request{}; request.nr = __NR_recvfrom;
        request.args[0] = fd; request.args[1] = buffer; request.args[2] = size;
        return Unix(request, memory).value_or(-ENOSYS);
    }
    if (d->file->proc) {
        auto& p = *d->file->proc;
        uint64_t at = positional ? offset : p.offset;
        size = at >= p.data.size() ? 0 : std::min<uint64_t>(size, p.data.size() - at);
        if (size && !memory.Write(buffer, p.data.data() + at, size)) return -EFAULT;
        if (!positional) p.offset += size;
        return size;
    }
    if (!size) return 0;
    size = std::min(size, kIoLimit);
    if (d->file->kind == OpenFile::kInput) return positional ? -ESPIPE : 0;
    if (!d->file->host) return -EBADF;
    std::vector<uint8_t> bytes(size);
    // Probe the destination before consuming bytes/offsets. Cross-page partial
    // I/O is bounded to a whole validated buffer for this implementation.
    if (!memory.Read(buffer, bytes.data(), size) || !memory.Write(buffer, bytes.data(), size)) return -EFAULT;
    ssize_t n = positional ? pread(d->file->host->fd, bytes.data(), size, offset)
                           : read(d->file->host->fd, bytes.data(), size);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !(d->file->flags & O_NONBLOCK)) return kBlocked;
    if (n < 0) return -errno;
    return memory.Write(buffer, bytes.data(), n) ? n : -EFAULT;
}
long FileTable::Write(int fd, uintptr_t buffer, size_t size, const GuestMemory& memory,
                      bool positional, uint64_t offset) {
    const auto* d = Get(fd);
    if (!d || (d->file->flags & O_ACCMODE) == O_RDONLY || (d->file->flags & O_PATH)) return -EBADF;
    if (!size) return 0;
    size = std::min(size, kIoLimit);
    std::vector<uint8_t> bytes(size);
    if (!memory.Read(buffer, bytes.data(), size)) return -EFAULT;
    if (d->file->unix_socket) {
        if (positional) return -ESPIPE;
        auto socket = d->file->unix_socket;
        long result = UnixSend(socket, socket->peer.lock(), std::move(bytes), {}, 0);
        return result == -EAGAIN && !(d->file->flags & O_NONBLOCK) ? kBlocked : result;
    }
    if (d->file->kind == OpenFile::kOutput || d->file->kind == OpenFile::kError) {
        if (positional) return -ESPIPE;
        auto& sink = d->file->kind == OpenFile::kOutput ? output->out : output->err;
        if (output->out.size() + output->err.size() > output_limit || size > output_limit - output->out.size() - output->err.size()) return -EFBIG;
        sink.append(reinterpret_cast<char*>(bytes.data()), size); return size;
    }
    if (!d->file->host) return -EBADF;
    struct stat st{};
    if (fstat(d->file->host->fd, &st) < 0) return -errno;
    if (S_ISREG(st.st_mode)) {
        int64_t start = (d->file->flags & O_APPEND) ? st.st_size : positional ? int64_t(offset) : lseek(d->file->host->fd, 0, SEEK_CUR);
        if (start < 0 || size > uint64_t(INT64_MAX) - start) return -EFBIG;
        int rc = vfs->CheckGrowth(d->file->host->fd, std::max<uint64_t>(st.st_size, start + size)); if (rc < 0) return rc;
        rc = ClearWritePrivilegeBits(d->file->host->fd); if (rc < 0) return rc;
    }
    ssize_t n = positional ? pwrite(d->file->host->fd, bytes.data(), size, offset)
                           : write(d->file->host->fd, bytes.data(), size);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !(d->file->flags & O_NONBLOCK)) return kBlocked;
    return Result(n);
}
long FileTable::Ioctl(int fd, unsigned long command, uintptr_t argument, const GuestMemory& memory) {
    const auto* d = Get(fd);
    if (!d) return -EBADF;
    if (command == FIONBIO) {
        int value;
        if (!memory.Read(argument,&value,sizeof(value))) return -EFAULT;
        int flags=(d->file->flags & ~O_NONBLOCK) | (value ? O_NONBLOCK : 0);
        if (d->file->host && fcntl(d->file->host->fd,F_SETFL,
                flags | (d->file->pollable ? O_NONBLOCK : 0)) < 0) return -errno;
        d->file->flags=flags;
        return 0;
    }
    if (d->file->unix_socket) {
        if (command == FIONREAD) {
            int bytes = 0;
            for (const auto& message : d->file->unix_socket->messages) {
                bytes += message.data.size() - message.offset;
                if (d->file->unix_socket->type != SOCK_STREAM) break;
            }
            return memory.Write(argument, &bytes, sizeof(bytes)) ? 0 : -EFAULT;
        }
        return -ENOTTY;
    }
    const int host = HostFd(fd);
    if (host < 0) return -ENOTTY;
    // Only explicitly translated ioctls can reach the host. In particular,
    // pointer-bearing, tty injection and arbitrary driver commands never do.
    switch (command) {
        case TCGETS: {
            KernelTermios value{};
            if (syscall(__NR_ioctl, host, TCGETS, &value) < 0) return -errno;
            return memory.Write(argument, &value, sizeof(value)) ? 0 : -EFAULT;
        }
        case TCSETS: case TCSETSW: case TCSETSF: {
            KernelTermios value{};
            if (!memory.Read(argument, &value, sizeof(value))) return -EFAULT;
            return Result(syscall(__NR_ioctl, host, command, &value));
        }
        case TIOCGWINSZ: case TIOCSWINSZ: {
            winsize value{};
            if (command == TIOCSWINSZ && !memory.Read(argument, &value, sizeof(value))) return -EFAULT;
            if (ioctl(host, command, &value) < 0) return -errno;
            return command == TIOCSWINSZ || memory.Write(argument, &value, sizeof(value)) ? 0 : -EFAULT;
        }
        case TIOCGPTN:
            if (!d->file->pty_master || !d->file->terminal) return -ENOTTY;
            return memory.Write(argument, &d->file->terminal->number, 4) ? 0 : -EFAULT;
        case FIONREAD: {
            int value = 0;
            if (ioctl(host, command, &value) < 0) return -errno;
            return memory.Write(argument, &value, sizeof(value)) ? 0 : -EFAULT;
        }
        case TIOCSPTLCK: {
            if (!d->file->pty_master || !d->file->terminal) return -ENOTTY;
            int value;
            if (!memory.Read(argument, &value, sizeof(value))) return -EFAULT;
            if (ioctl(host, command, &value) < 0) return -errno;
            d->file->terminal->locked = value != 0; return 0;
        }
        case TIOCGPGRP:
            if (!d->file->terminal) return -ENOTTY;
            if (!d->file->pty_master && controlling_terminal() != d->file->terminal) return -ENOTTY;
            return memory.Write(argument, &d->file->terminal->foreground_group, 4) ? 0 : -EFAULT;
        case TIOCSPGRP: {
            int group;
            if (!memory.Read(argument, &group, sizeof(group))) return -EFAULT;
            if (group <= 0) return -EINVAL;
            if (!d->file->terminal) return -ENOTTY;
            d->file->terminal->foreground_group = group; return 0;
        }
        case TIOCGSID:
            if (!d->file->terminal || !d->file->terminal->session) return -ENOTTY;
            if (!d->file->pty_master && controlling_terminal() != d->file->terminal) return -ENOTTY;
            return memory.Write(argument, &d->file->terminal->session, 4) ? 0 : -EFAULT;
        case TIOCSCTTY:
            if (!d->file->terminal) return -ENOTTY;
            controlling_terminal_ = d->file->terminal; return 0;
        case TIOCNOTTY:
            if (!d->file->terminal) return -ENOTTY;
            controlling_terminal_.reset(); return 0;
        case TCFLSH: return Result(ioctl(host, command, argument));
        default: return -ENOTTY;
    }
}
std::optional<long> FileTable::Handle(const SyscallRequest& request, const GuestMemory& memory) {
    Credentials access_credentials = CurrentCredentials();
    if (request.nr == 48 || (request.nr == 439 && !(request.args[3] & AT_EACCESS))) {
        access_credentials.fsuid = access_credentials.uid; access_credentials.fsgid = access_credentials.gid;
    }
    CredentialScope access_scope(access_credentials);
    if (auto result = Proc(request, memory)) return result;
    const auto& a = request.args;
    auto store = [&](uintptr_t pointer, const void* value, size_t size) -> long {
        return memory.Write(pointer, value, size) ? 0 : -EFAULT;
    };
    switch (request.nr) {
        case 19: { // eventfd2; the guest sees a virtual descriptor, never the host fd.
            if (a[1] & ~(EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE)) return -EINVAL;
            int fd = eventfd(static_cast<unsigned>(a[0]), a[1] | EFD_CLOEXEC | EFD_NONBLOCK);
            if (fd < 0) return -errno;
            auto file = std::make_shared<OpenFile>();
            file->host = std::make_shared<HostFile>(fd); file->pollable = true;
            file->flags = O_RDWR | ((a[1] & EFD_NONBLOCK) ? O_NONBLOCK : 0);
            return Install(file, a[1] & EFD_CLOEXEC);
        }
        case 17: { // getcwd
            if (cwd_handle) { int rc = vfs->PathFd(cwd_handle->fd, &cwd); if (rc < 0) return rc; }
            if (a[1] < cwd.size() + 1) return -ERANGE;
            return memory.Write(a[0], cwd.c_str(), cwd.size() + 1) ? static_cast<long>(cwd.size() + 1) : -EFAULT;
        }
        case 23: case 24: { // dup, dup3
            const auto* d = Get(a[0]); if (!d) return -EBADF;
            if (request.nr == 23) return Install(d->file);
            if (a[0] == a[1] || (a[2] & ~O_CLOEXEC)) return -EINVAL;
            if (a[1] >= 256) return -EBADF;
            Descriptor copy{d->file, bool(a[2] & O_CLOEXEC)};
            const auto* previous = Get(a[1]);
            if (previous && previous->file->host) vfs->ReleaseLocks(lock_owner, previous->file->host->fd);
            descriptors_[a[1]] = copy; unix_namespace->dirty = true; return a[1];
        }
        case 25: { // fcntl
            auto it = descriptors_.find(a[0]); if (it == descriptors_.end()) return -EBADF;
            auto& d = it->second;
            switch (a[1]) {
                case F_DUPFD: case F_DUPFD_CLOEXEC: return Install(d.file, a[1] == F_DUPFD_CLOEXEC, a[2]);
                case F_GETFD: return d.cloexec ? FD_CLOEXEC : 0;
                case F_SETFD: d.cloexec = a[2] & FD_CLOEXEC; return 0;
                case F_GETFL: return d.file->flags;
                case F_GETLK: case F_SETLK: case F_SETLKW: {
                    if (!d.file->host || (d.file->flags & O_PATH)) return -EBADF;
                    struct flock lock{};
                    if (!memory.Read(a[2], &lock, sizeof(lock))) return -EFAULT;
                    if (a[1] != F_GETLK && ((lock.l_type == F_WRLCK && (d.file->flags & O_ACCMODE) == O_RDONLY) ||
                        (lock.l_type == F_RDLCK && (d.file->flags & O_ACCMODE) == O_WRONLY))) return -EBADF;
                    long rc = vfs->Lock(d.file->host->fd, lock_owner, a[1], &lock);
                    if (rc == -EAGAIN && a[1] == F_SETLKW) return kBlocked;
                    if (rc < 0) return rc;
                    return a[1] != F_GETLK || memory.Write(a[2], &lock, sizeof(lock)) ? 0 : -EFAULT;
                }
                case F_SETFL: {
                    int flags = (d.file->flags & ~(O_APPEND | O_NONBLOCK)) | (a[2] & (O_APPEND | O_NONBLOCK));
                    if (d.file->host && fcntl(d.file->host->fd, F_SETFL,
                         flags | (d.file->pollable ? O_NONBLOCK : 0)) < 0) return -errno;
                    d.file->flags = flags; return 0;
                }
                default: return -EINVAL;
            }
        }
        case 29: return Ioctl(a[0], a[1], a[2], memory);
        case 32: { // flock is tied to a confined open file description.
            int fd = HostFd(a[0]); if (fd < 0) return -EBADF;
            if ((a[1] & ~(LOCK_SH | LOCK_EX | LOCK_UN | LOCK_NB)) ||
                ((a[1] & ~LOCK_NB) != LOCK_SH && (a[1] & ~LOCK_NB) != LOCK_EX && (a[1] & ~LOCK_NB) != LOCK_UN)) return -EINVAL;
            int rc = flock(fd, a[1] | LOCK_NB);
            if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !(a[1] & LOCK_NB)) return kBlocked;
            return Result(rc);
        }
        case 34: { // mkdirat
            std::string path; int rc = Path(a[0], a[1], memory, &path, false);
            return rc < 0 ? rc : vfs->Mkdir(path, a[2] & ~mask);
        }
        case 35: {
            if (a[2] & ~AT_REMOVEDIR) return -EINVAL;
            std::string path; int rc = Path(a[0], a[1], memory, &path, false);
            return rc < 0 ? rc : vfs->Unlink(path, a[2] & AT_REMOVEDIR);
        }
        case 36: {
            std::string target, path;
            if (!memory.ReadString(a[0], &target)) return -EFAULT;
            int rc = Path(a[1], a[2], memory, &path, false);
            return rc < 0 ? rc : vfs->Symlink(target, path);
        }
        case 37: {
            if (a[4] & ~AT_SYMLINK_FOLLOW) return -EINVAL;
            std::string source, target;
            int rc = Path(a[0], a[1], memory, &source, a[4] & AT_SYMLINK_FOLLOW); if (rc < 0) return rc;
            rc = Path(a[2], a[3], memory, &target, false);
            return rc < 0 ? rc : vfs->Link(source, target, a[4] & AT_SYMLINK_FOLLOW);
        }
        case 43: case 44: {
            struct statfs info{};
            int fd = -1;
            if (request.nr == 43) {
                std::string path; int rc = Path(AT_FDCWD, a[0], memory, &path); if (rc < 0) return rc;
                fd = vfs->Open(path, O_RDONLY | O_NONBLOCK); if (fd < 0) return fd;
            } else { fd = HostFd(a[0]); if (fd < 0) return -EBADF; }
            int rc = vfs->StatFs(fd, &info);
            if (request.nr == 43) close(fd);
            return rc < 0 ? rc : store(a[1], &info, sizeof(info));
        }
        case 45: {
            if (static_cast<int64_t>(a[1]) < 0) return -EINVAL;
            std::string path; int rc = Path(AT_FDCWD, a[0], memory, &path); if (rc < 0) return rc;
            int fd = vfs->Open(path, O_WRONLY); if (fd < 0) return fd;
            HostFile owner(fd); rc = vfs->CheckGrowth(fd, a[1]); if (rc == 0) rc = ClearWritePrivilegeBits(fd);
            return rc < 0 ? rc : Result(ftruncate(fd, a[1]));
        }
        case 52: {
            const auto* d=Get(a[0]); if (!d) return -EBADF;
            if (d->file->terminal) return TerminalChmod(*d->file->terminal,a[1]);
            int fd=HostFd(a[0]); return fd<0?-EBADF:vfs->ChmodFd(fd,a[1]);
        }
        case 53: {
            std::string path; int rc = Path(a[0], a[1], memory, &path);
            if (rc<0) return rc;
            if (auto terminal=TerminalAt(path)) return TerminalChmod(*terminal,a[2]);
            return vfs->Chmod(path,a[2]);
        }
        case 54: case 55: {
            const unsigned uid = a[request.nr == 54 ? 2 : 1], gid = a[request.nr == 54 ? 3 : 2];
            if (request.nr == 55) {
                const auto* d=Get(a[0]); if (!d) return -EBADF;
                if (d->file->terminal) return TerminalChown(*d->file->terminal,uid,gid);
                int fd=HostFd(a[0]); return fd<0?-EBADF:vfs->ChownFd(fd,uid,gid);
            }
            if (a[4] & ~AT_SYMLINK_NOFOLLOW) return -EINVAL;
            std::string path; int rc = Path(a[0], a[1], memory, &path, !(a[4] & AT_SYMLINK_NOFOLLOW));
            if (rc<0) return rc;
            if (auto terminal=TerminalAt(path)) return TerminalChown(*terminal,uid,gid);
            return vfs->Chown(path,uid,gid,!(a[4] & AT_SYMLINK_NOFOLLOW));
        }
        case 88: {
            timespec times[2];
            if (a[2] && !memory.Read(a[2], times, sizeof(times))) return -EFAULT;
            if (a[3] & ~AT_SYMLINK_NOFOLLOW) return -EINVAL;
            if (!a[1]) { int fd = HostFd(a[0]); return fd < 0 ? -EBADF : vfs->TimesFd(fd, a[2] ? times : nullptr); }
            std::string path; int rc = Path(a[0], a[1], memory, &path, !(a[3] & AT_SYMLINK_NOFOLLOW));
            return rc < 0 ? rc : vfs->Times(path, a[2] ? times : nullptr, !(a[3] & AT_SYMLINK_NOFOLLOW));
        }
        case 38: case 276: {
            if (request.nr == 276 && a[4]) return -EINVAL;
            std::string oldpath, newpath;
            int rc = Path(a[0], a[1], memory, &oldpath, false); if (rc < 0) return rc;
            rc = Path(a[2], a[3], memory, &newpath, false);
            return rc < 0 ? rc : vfs->Rename(oldpath, newpath);
        }
        case 46: {
            const auto* d = Get(a[0]); int fd = HostFd(a[0]);
            if (!d || fd < 0) return -EBADF;
            if ((d->file->flags & O_ACCMODE) == O_RDONLY || (d->file->flags & O_PATH) || static_cast<int64_t>(a[1]) < 0) return -EINVAL;
            int rc = vfs->CheckGrowth(fd, a[1]); if (rc == 0) rc = ClearWritePrivilegeBits(fd);
            return rc < 0 ? rc : Result(ftruncate(fd, a[1]));
        }
        case 48: case 439: { // faccessat / faccessat2
            if (request.nr == 439 && a[3] & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW)) return -EINVAL;
            bool follow = request.nr != 439 || !(a[3] & AT_SYMLINK_NOFOLLOW);
            std::string path; int rc = Path(a[0], a[1], memory, &path, follow);
            return rc < 0 ? rc : vfs->Access(path, a[2], follow);
        }
        case 49: case 50: { // chdir / fchdir
            std::string path;
            if (request.nr == 49) { int rc = Path(AT_FDCWD, a[0], memory, &path); if (rc < 0) return rc; }
            else { const auto* d = Get(a[0]); if (!d) return -EBADF; if (!d->file->directory) return -ENOTDIR; int rc = vfs->PathFd(d->file->host->fd, &path); if (rc < 0) return rc; }
            std::string canonical;
            int fd = vfs->Open(path, O_PATH | O_DIRECTORY, 0, &canonical);
            if (fd < 0) return fd;
            int rc = vfs->AccessFd(fd, X_OK); if (rc < 0) { close(fd); return rc; }
            cwd_handle = std::make_shared<HostFile>(fd); cwd = canonical; return 0;
        }
        case 56: { // openat
            std::string path; int rc = Path(a[0], a[1], memory, &path, !(a[2] & O_NOFOLLOW) && !((a[2] & O_CREAT) && (a[2] & O_EXCL))); if (rc < 0) return rc;
            const int flags = a[2];
            // Host O_ASYNC and O_DIRECT are deliberately outside this ABI.
            const int allowed = O_ACCMODE | O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC | O_APPEND |
                O_NONBLOCK | O_DSYNC | O_SYNC | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_PATH | O_LARGEFILE;
            if (flags & ~allowed) return -EINVAL;
            auto file = std::make_shared<OpenFile>();
            int host;
            if (path == "/dev/null" || path == "/dev/zero" || path == "/dev/urandom" || path == "/dev/random") {
                host = open(path.c_str(), (flags & (O_ACCMODE | O_NONBLOCK)) | O_CLOEXEC | O_NOCTTY);
                if (host < 0) return -errno;
                file->path = path;
            } else if (path == "/dev/ptmx") {
                int error = 0;
                file = NewTerminal(&error); if (!file) return error;
                file->flags = flags & ~(O_CLOEXEC | O_CREAT | O_EXCL | O_TRUNC);
                return Install(file, flags & O_CLOEXEC);
            } else if (path == "/dev/tty" || path.compare(0, 9, "/dev/pts/") == 0) {
                auto terminal = TerminalAt(path);
                if (!terminal) return path == "/dev/tty" ? -ENXIO : -ENOENT;
                if (terminal->locked) return -EIO;
                struct stat st{}; int rc=TerminalStat(*terminal,&st); if (rc<0) return rc;
                rc=CheckPermission(st,(flags&O_ACCMODE)==O_RDONLY?R_OK:(flags&O_ACCMODE)==O_WRONLY?W_OK:R_OK|W_OK);
                if (rc<0) return rc;
                host = open(terminal->host_slave.c_str(), (flags & O_ACCMODE) | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
                if (host < 0) return -errno;
                file->terminal = terminal; file->path = "/dev/pts/" + std::to_string(terminal->number);
                file->pollable = true;
            } else {
                host = vfs->Open(path, flags | O_NONBLOCK, a[3] & ~mask, &file->path);
                if (host < 0) return host;
            }
            file->host = std::make_shared<HostFile>(host);
            vfs->Track(file->host);
            struct stat st{};
            if (fstat(host, &st) < 0) return -errno;
            file->directory = S_ISDIR(st.st_mode);
            file->pollable |= S_ISFIFO(st.st_mode) || isatty(host);
            file->flags = flags & ~(O_CLOEXEC | O_CREAT | O_EXCL | O_TRUNC);
            return Install(file, flags & O_CLOEXEC);
        }
        case 57: {
            const auto* d = Get(a[0]); if (!d) return -EBADF;
            if (d->file->host) vfs->ReleaseLocks(lock_owner, d->file->host->fd);
            descriptors_.erase(a[0]); unix_namespace->dirty = true; return 0;
        }
        case 59: { // pipe2; broker endpoints are always nonblocking.
            if (a[1] & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
            int original[2]; if (!memory.Read(a[0], original, sizeof(original)) ||
                !memory.Write(a[0], original, sizeof(original))) return -EFAULT;
            int pipefd[2]; if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0) return -errno;
            int guest[2];
            for (int i = 0; i < 2; ++i) {
                auto file = std::make_shared<OpenFile>();
                file->host = std::make_shared<HostFile>(pipefd[i]);
                file->flags = (i ? O_WRONLY : O_RDONLY) | (a[1] & O_NONBLOCK);
                file->pollable = true;
                guest[i] = Install(file, a[1] & O_CLOEXEC);
            }
            if (guest[0] < 0 || guest[1] < 0) {
                if (guest[0] >= 0) descriptors_.erase(guest[0]);
                if (guest[1] >= 0) descriptors_.erase(guest[1]);
                return -EMFILE;
            }
            if (!memory.Write(a[0], guest, sizeof(guest))) {
                descriptors_.erase(guest[0]); descriptors_.erase(guest[1]); return -EFAULT;
            }
            return 0;
        }
        case 61: { // getdents64, with virtual inode identities and types.
            int fd = HostFd(a[0]); if (fd < 0) return -EBADF;
            std::vector<uint8_t> data(std::min<size_t>(a[2], kIoLimit));
            long n = syscall(__NR_getdents64, fd, data.data(), data.size());
            if (n < 0) return -errno;
            struct Dirent { uint64_t inode; int64_t offset; uint16_t length; uint8_t type; char name[]; };
            size_t output = 0;
            const auto* d = Get(a[0]);
            std::string directory;
            if (d && d->file->directory) vfs->PathFd(fd, &directory);
            for (size_t at = 0; at < static_cast<size_t>(n);) {
                auto* entry = reinterpret_cast<Dirent*>(data.data() + at);
                if (entry->length < 20 || entry->length > n - at) return -EIO;
                size_t length = entry->length;
                if (strcmp(entry->name, ".goblin-inodes")) {
                    struct stat st{};
                    if (!directory.empty() && vfs->Stat(directory + "/" + entry->name, &st, false) == 0) {
                        entry->inode = st.st_ino; entry->type = (st.st_mode & S_IFMT) >> 12;
                    }
                    memmove(data.data() + output, entry, length); output += length;
                }
                at += length;
            }
            return memory.Write(a[1], data.data(), output) ? static_cast<long>(output) : -EFAULT;
        }
        case 62: {
            const auto* d = Get(a[0]); if (!d) return -EBADF;
            if (!d->file->host) return -ESPIPE;
            return Result(lseek(d->file->host->fd, a[1], a[2]));
        }
        case 63: case 67: return Read(a[0], a[1], a[2], memory, request.nr == 67, a[3]);
        case 64: case 68: return Write(a[0], a[1], a[2], memory, request.nr == 68, a[3]);
        case 65: case 66: {
            if (a[2] > 1024) return -EINVAL;
            struct Iov { uint64_t base, length; };
            std::vector<Iov> iov(a[2]);
            if (!memory.Read(a[1], iov.data(), iov.size() * sizeof(Iov))) return -EFAULT;
            long total = 0;
            for (const auto& v : iov) {
                long n = request.nr == 65 ? Read(a[0], v.base, v.length, memory)
                                         : Write(a[0], v.base, v.length, memory);
                if (n < 0) return total ? total : n;
                total += n;
                if (static_cast<uint64_t>(n) < v.length) break;
            }
            return total;
        }
        case 78: { // readlinkat
            if (!a[3]) return -EINVAL;
            std::string path, target; int rc = Path(a[0], a[1], memory, &path, false); if (rc < 0) return rc;
            long n;
            if (path == "/proc/self/exe") { target = executable; n = target.size(); }
            else n = vfs->Readlink(path, &target);
            if (n < 0) return n;
            n = std::min<unsigned long>(n, a[3]);
            return memory.Write(a[2], target.data(), n) ? n : -EFAULT;
        }
        case 79: case 80: { // newfstatat / fstat
            struct stat st{}; static_assert(sizeof(st) == 128, "ARM64 stat ABI");
            uintptr_t destination = a[1];
            int rc = 0;
            if (request.nr == 79) {
                destination = a[2];
                if (a[3] & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT)) return -EINVAL;
                std::string raw;
                if (!memory.ReadString(a[1], &raw)) return -EFAULT;
                if (raw.empty() && (a[3] & AT_EMPTY_PATH)) {
                    int fd = HostFd(a[0]); if (fd < 0) return -EBADF;
                    rc = vfs->StatFd(fd, &st);
                    const auto* d=Get(a[0]); if (d && d->file->terminal) TerminalMode(*d->file->terminal,&st);
                } else {
                    std::string path; rc = Path(a[0], a[1], memory, &path, !(a[3] & AT_SYMLINK_NOFOLLOW));
                    if (rc == 0) {
                        if (auto terminal=TerminalAt(path)) rc=TerminalStat(*terminal,&st);
                        else rc = vfs->Stat(path, &st, !(a[3] & AT_SYMLINK_NOFOLLOW));
                    }
                }
            } else {
                const auto* d = Get(a[0]); if (!d) return -EBADF;
                if (d->file->host) { rc = vfs->StatFd(d->file->host->fd, &st);
                    if (d->file->terminal) TerminalMode(*d->file->terminal,&st);
                }
                else { st.st_mode = (d->file->unix_socket ? S_IFSOCK : S_IFCHR) | 0666; st.st_ino = d->file->identity; st.st_nlink = 1; st.st_blksize = 4096; }
            }
            if (rc < 0) return rc;
            return store(destination, &st, sizeof(st));
        }
        case 81: return vfs->Sync();
        case 82: case 83: {
            int fd = HostFd(a[0]); if (fd < 0) return -EBADF;
            return Result(request.nr == 82 ? fsync(fd) : fdatasync(fd));
        }
        case 166: { unsigned old = mask; mask = a[0] & 0777; return old; }
        default: return Network(request, memory);
    }
}
}  // namespace goblin
