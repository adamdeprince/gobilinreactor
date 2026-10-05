// Runs inside Linux/UML, never as an Android process. Linux supplies PTYs,
// credentials, signals, filesystems, scheduling and memory management.
#include "protocol.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <grp.h>
#include <linux/ipv6.h>
#include <map>
#include <memory>
#include <net/if.h>
#include <net/route.h>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>
#include "guest-dns.h"

using namespace goblin_uml;
namespace {
bool rescue = false;
void Require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what + ": " + strerror(errno));
}
struct Fd {
    int fd;
    explicit Fd(int n) : fd(n) { Require(n >= 0, "open"); }
    ~Fd() { close(fd); }
};
void Read(int fd, void* buffer, size_t size) {
    char* p = static_cast<char*>(buffer);
    while (size) {
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (!n) { errno = EIO; }
        Require(n > 0, "reading archive"); p += n; size -= n;
    }
}
void Write(int fd, const void* buffer, size_t size) {
    const char* p = static_cast<const char*>(buffer);
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        Require(n > 0, "write"); p += n; size -= n;
    }
}
void Directory(const std::string& path, mode_t mode = 0755) {
    Require(mkdir(path.c_str(), mode) == 0 || errno == EEXIST, "mkdir " + path);
}
bool Exists(const std::string& path) { struct stat st{}; return lstat(path.c_str(), &st) == 0; }
void File(const std::string& path, const std::string& text, mode_t mode = 0644) {
    Fd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode));
    Write(fd.fd, text.data(), text.size()); Require(fchmod(fd.fd, mode) == 0, path);
}
void Copy(const std::string& source, const std::string& destination, mode_t mode = 0644) {
    Fd in(open(source.c_str(), O_RDONLY | O_CLOEXEC));
    Fd out(open(destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode));
    char bytes[65536]; ssize_t n;
    while ((n = read(in.fd, bytes, sizeof(bytes))) != 0) {
        if (n < 0 && errno == EINTR) continue;
        Require(n > 0, source); Write(out.fd, bytes, n);
    }
    Require(fchmod(out.fd, mode) == 0, destination);
}
int Run(const std::vector<std::string>& arguments) {
    std::vector<char*> args;
    for (const auto& value : arguments) args.push_back(const_cast<char*>(value.c_str()));
    args.push_back(nullptr);
    pid_t pid = fork(); Require(pid >= 0, "fork setup command");
    if (!pid) { execv(args[0], args.data()); perror(args[0]); _exit(127); }
    int status; while (waitpid(pid, &status, 0) < 0) Require(errno == EINTR, "waitpid");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
void CheckedRun(const std::vector<std::string>& args) {
    if (Run(args)) throw std::runtime_error("Guest setup failed: " + args.front());
}
bool Safe(const std::string& path) {
    if (path.find('\0') != std::string::npos || (!path.empty() && path.front() == '/')) return false;
    size_t at = 0;
    while (at < path.size()) {
        size_t end = path.find('/', at); if (end == std::string::npos) end = path.size();
        if (path.substr(at, end - at) == "..") return false;
        at = end + 1;
    }
    return true;
}
void Unpack(const std::string& archive, const std::string& root) {
    Fd in(open(archive.c_str(), O_RDONLY | O_CLOEXEC)); char magic[8]; Read(in.fd, magic, 8);
    bool legacy = !memcmp(magic, "GOBLINFS", 8);
    Require(legacy || !memcmp(magic, "GOBLINU2", 8), "unknown archive format");
    uint32_t count = 0; if (legacy) Read(in.fd, &count, 4);
    for (uint64_t index = 0; !legacy || index < count; ++index) {
        Entry entry{};
        if (legacy) { Read(in.fd, &entry.name_size, 4); Read(in.fd, &entry.mode, 4); Read(in.fd, &entry.size, 8); }
        else { Read(in.fd, &entry, sizeof(entry)); if (entry.name_size == UINT32_MAX) break; }
        std::string name(entry.name_size, '\0'); Read(in.fd, name.data(), name.size());
        Require(Safe(name), "invalid archive path");
        const std::string path = root + (name.empty() ? "" : "/" + name);
        if (!(entry.flags & MetadataOnly)) {
            if (entry.flags & HardLink || S_ISLNK(entry.mode)) {
                std::string target(entry.size, '\0'); Read(in.fd, target.data(), target.size());
                if (entry.flags & HardLink) {
                    Require(Safe(target), "invalid hard link");
                    Require(link((root + "/" + target).c_str(), path.c_str()) == 0, path);
                } else Require(symlink(target.c_str(), path.c_str()) == 0, path);
            } else if (S_ISDIR(entry.mode)) Directory(path, entry.mode & 07777);
            else if (S_ISREG(entry.mode)) {
                Fd out(open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600));
                uint64_t remaining = entry.size; char bytes[65536];
                while (remaining) {
                    size_t n = std::min<uint64_t>(remaining, sizeof(bytes)); Read(in.fd, bytes, n);
                    if (std::all_of(bytes, bytes + n, [](char c) { return !c; })) Require(lseek(out.fd, n, SEEK_CUR) >= 0, path);
                    else Write(out.fd, bytes, n);
                    remaining -= n;
                }
                Require(ftruncate(out.fd, entry.size) == 0, path);
            } else Require(mknod(path.c_str(), entry.mode, 0) == 0, path);
        }
        Require(lchown(path.c_str(), entry.uid, entry.gid) == 0, "ownership " + path);
        if (!S_ISLNK(entry.mode)) Require(chmod(path.c_str(), entry.mode & 07777) == 0, "mode " + path);
        if (!legacy) {
            timespec times[] = {{entry.seconds, long(entry.nanoseconds)}, {entry.seconds, long(entry.nanoseconds)}};
            Require(utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0, "timestamp " + path);
        }
    }
    puts("Debian files and ownership restored."); fflush(stdout);
}
void Mount(const char* source, const char* path, const char* type, unsigned long flags = 0, const char* data = nullptr) {
    Directory(path); Require(mount(source, path, type, flags, data) == 0 || errno == EBUSY, std::string("mount ") + path);
}
void Network() {
    Fd sock(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    auto address = [&](const char* name, unsigned long operation, const char* value) {
        ifreq req{}; strncpy(req.ifr_name, name, IFNAMSIZ - 1);
        auto* a = reinterpret_cast<sockaddr_in*>(&req.ifr_addr); a->sin_family = AF_INET; inet_pton(AF_INET, value, &a->sin_addr);
        Require(ioctl(sock.fd, operation, &req) == 0, "configure network address");
    };
    auto up = [&](const char* name) {
        ifreq req{}; strncpy(req.ifr_name, name, IFNAMSIZ - 1); Require(ioctl(sock.fd, SIOCGIFFLAGS, &req) == 0, name);
        req.ifr_flags |= IFF_UP; Require(ioctl(sock.fd, SIOCSIFFLAGS, &req) == 0, name);
    };
    address("lo", SIOCSIFADDR, "127.0.0.1"); up("lo");
    address("lo:goblin", SIOCSIFADDR, "10.0.2.3");
    address("lo:goblin", SIOCSIFNETMASK, "255.255.255.255");
    address("vec0", SIOCSIFADDR, "10.0.2.15"); address("vec0", SIOCSIFNETMASK, "255.255.255.0"); up("vec0");
    rtentry route{};
    reinterpret_cast<sockaddr_in*>(&route.rt_dst)->sin_family = AF_INET;
    reinterpret_cast<sockaddr_in*>(&route.rt_genmask)->sin_family = AF_INET;
    auto* gateway = reinterpret_cast<sockaddr_in*>(&route.rt_gateway); gateway->sin_family = AF_INET; inet_pton(AF_INET, "10.0.2.2", &gateway->sin_addr);
    route.rt_flags = RTF_UP | RTF_GATEWAY; route.rt_dev = const_cast<char*>("vec0");
    Require(ioctl(sock.fd, SIOCADDRT, &route) == 0 || errno == EEXIST, "default route");
    Fd v6(socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    in6_ifreq a6{}; a6.ifr6_ifindex = if_nametoindex("vec0"); a6.ifr6_prefixlen = 64;
    inet_pton(AF_INET6, "fd00:676f:626c::15", &a6.ifr6_addr);
    Require(ioctl(v6.fd, SIOCSIFADDR, &a6) == 0 || errno == EEXIST, "IPv6 address");
    in6_rtmsg r6{}; r6.rtmsg_ifindex = a6.ifr6_ifindex; r6.rtmsg_metric = 1;
    r6.rtmsg_flags = RTF_UP | RTF_GATEWAY;
    inet_pton(AF_INET6, "fe80::1", &r6.rtmsg_gateway);
    Require(ioctl(v6.fd, SIOCADDRT, &r6) == 0 || errno == EEXIST, "IPv6 default route");
}
void Boot() {
    umask(022); setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("HOME", "/root", 1); setenv("DEBIAN_FRONTEND", "noninteractive", 1);
    Mount("devtmpfs", "/dev", "devtmpfs"); Mount("proc", "/proc", "proc"); Mount("sysfs", "/sys", "sysfs");
    // An isolated Android host receives open files, not access to app-private
    // paths. The second read-only block device contains this boot's APK assets.
    Mount("tmpfs", "/host", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=0");
    Unpack("/dev/ubdb", "/host");
    Directory("/newroot");
    const bool new_root = Exists("/host/new-root");
    if (new_root) {
        puts("Creating the Linux filesystem…"); fflush(stdout);
        CheckedRun({"/sbin/mke2fs", "-q", "-t", "ext4", "-F", "-m", "0", "-E", "lazy_itable_init=1,lazy_journal_init=1", "/dev/ubda"});
    } else {
        bool grow = Exists("/host/grow-root");
        int status = Run({"/sbin/e2fsck", grow ? "-pf" : "-p", "/dev/ubda"});
        if (status > 1) throw std::runtime_error("Linux filesystem needs repair; the disk image has been retained.");
        if (grow) CheckedRun({"/sbin/resize2fs", "/dev/ubda"});
    }
    Mount("/dev/ubda", "/newroot", "ext4", MS_NOATIME);
    if (new_root) Unpack("/dev/ubdc", "/newroot");
    Require(mount(nullptr, "/host", nullptr, MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV, nullptr) == 0, "protect deployment assets");
    Directory("/newroot/run"); Mount("tmpfs", "/newroot/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=0");
    Directory("/newroot/run/goblin-host"); Require(mount("/host", "/newroot/run/goblin-host", nullptr, MS_MOVE, nullptr) == 0, "move deployment mount");
    Require(chdir("/newroot") == 0 && chroot(".") == 0 && chdir("/") == 0, "enter Debian root");
    Mount("devtmpfs", "/dev", "devtmpfs"); Directory("/dev/pts");
    Mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "newinstance,ptmxmode=0666,mode=0620,gid=5");
    unlink("/dev/ptmx"); Require(symlink("pts/ptmx", "/dev/ptmx") == 0, "ptmx");
    unlink("/dev/fd"); symlink("/proc/self/fd", "/dev/fd");
    unlink("/dev/stdin"); symlink("/proc/self/fd/0", "/dev/stdin");
    unlink("/dev/stdout"); symlink("/proc/self/fd/1", "/dev/stdout");
    unlink("/dev/stderr"); symlink("/proc/self/fd/2", "/dev/stderr");
    Mount("proc", "/proc", "proc"); Mount("sysfs", "/sys", "sysfs");
    Directory("/dev/shm", 01777); Mount("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777,size=0");
    Directory("/sys/fs/cgroup"); Mount("cgroup2", "/sys/fs/cgroup", "cgroup2");
    sethostname("goblin", 6); Network();
    // Translate known seed/legacy defaults only during initial import. Once
    // the disk exists, deployment tracks DNS like other editable config;
    // even an empty or deleted resolv.conf must survive a boot/APK update.
    struct stat resolver_stat{};
    if (new_root && lstat("/etc/resolv.conf", &resolver_stat) == 0 &&
        S_ISREG(resolver_stat.st_mode) && resolver_stat.st_nlink == 1 &&
        (resolver_stat.st_mode & 07777) == 0644 && !resolver_stat.st_uid && !resolver_stat.st_gid) {
        std::string resolver;
        FILE* f = fopen("/etc/resolv.conf", "r");
        if (f) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof(b), f))) resolver.append(b, n); fclose(f); }
        if (resolver == "nameserver 127.0.0.53\noptions timeout:5 attempts:2\n" ||
            resolver == "nameserver 1.1.1.1\nnameserver 8.8.8.8\noptions timeout:3 attempts:2\n")
            File("/etc/resolv.conf", "nameserver 10.0.2.3\n");
    }
    CheckedRun({"/bin/sh", "/usr/local/sbin/goblin-bootstrap"});
    if (!getpwnam("goblin")) CheckedRun({"/usr/sbin/useradd", "--create-home", "--user-group", "--shell", "/bin/bash", "-K", "HOME_MODE=0700", "goblin"});
    Directory("/var/lib/goblin"); Directory("/var/lib/goblin/deployment", 0700);
    DIR* assets = opendir("/run/goblin-host/deployment"); Require(assets != nullptr, "guest deployment assets");
    while (auto* item = readdir(assets)) {
        std::string name = item->d_name; if (name == "." || name == "..") continue;
        Copy("/run/goblin-host/deployment/" + name, "/var/lib/goblin/deployment/" + name, 0600);
    }
    closedir(assets); CheckedRun({"/bin/sh", "/var/lib/goblin/deployment/configure.sh"});
    Directory("/run/goblin");
    Copy("/proc/self/exe", "/run/goblin/agent", 0755);
    Directory("/run/systemd"); Directory("/run/systemd/system");
    Directory("/run/systemd/system/multi-user.target.wants");
    File("/run/systemd/system/goblin-terminal.service",
        "[Unit]\nDescription=Goblin terminal and Android DNS bridge\n"
        "After=local-fs.target systemd-user-sessions.service systemd-logind.service\n"
        "Wants=systemd-user-sessions.service systemd-logind.service\nBefore=multi-user.target\nStartLimitIntervalSec=0\n"
        "[Service]\nExecStart=/run/goblin/agent --agent\nRestart=on-failure\n"
        "KillMode=process\nTimeoutStopSec=infinity\nTasksMax=infinity\n"
        "[Install]\nWantedBy=multi-user.target\n");
    Require(symlink("../goblin-terminal.service", "/run/systemd/system/multi-user.target.wants/goblin-terminal.service") == 0,
            "enable terminal service");
    File("/var/lib/goblin/uml-imported", "Linux UML filesystem migration complete\n");
    sync();
}
struct Buffer {
    std::string bytes; size_t offset = 0;
    bool empty() const { return offset == bytes.size(); }
    void add(const void* p, size_t n) { bytes.append(static_cast<const char*>(p), n); }
    void flush(int fd, bool pty = false) {
        if (empty()) return;
        ssize_t n = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (n > 0) offset += n;
        else if (n < 0 && errno != EINTR && errno != EAGAIN && !(pty && errno == EIO)) throw std::runtime_error("Terminal transport disconnected");
        if (empty()) { bytes.clear(); offset = 0; }
    }
};
Buffer transport;
void Send(uint32_t op, uint32_t id, const void* data = nullptr, size_t size = 0) {
    Header h{kMagic, op, id, uint32_t(size)}; transport.add(&h, sizeof(h)); if (size) transport.add(data, size);
}
void Failure(uint32_t id, const std::string& text) { Send(Error, id, text.data(), text.size()); }
struct Terminal { pid_t pid = -1; int fd = -1; Buffer input; };
std::map<uint32_t, Terminal> terminals;
std::unique_ptr<GuestDns> dns;
volatile sig_atomic_t stopping = 0;
void Stop(int) { stopping = 1; }
void Start(uint32_t id, const std::string& payload, bool execute) {
    try {
        if (terminals.count(id)) throw std::runtime_error("Terminal already exists");
        if (!execute && payload.empty()) throw std::runtime_error("Missing account");
        std::string username = execute || rescue ? "root" : payload.substr(1);
        if (username.empty() || username.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
            throw std::runtime_error("Invalid account name");
        if (!execute && payload[0] && !getpwnam(username.c_str())) CheckedRun({"/usr/sbin/useradd", "--create-home", "--user-group", "--shell", "/bin/bash", "-K", "HOME_MODE=0700", username});
        passwd* pw = getpwnam(username.c_str()); if (!pw) throw std::runtime_error("Account does not exist: " + username);
        uid_t uid = pw->pw_uid; gid_t gid = pw->pw_gid; std::string home = pw->pw_dir, shell = pw->pw_shell;
        int master; winsize dimensions{24, 80, 0, 0};
        pid_t pid = forkpty(&master, nullptr, nullptr, &dimensions); Require(pid >= 0, "open terminal");
        if (!pid) {
            // No Goblin resource limits. Normal Linux defaults and user-set
            // rlimits apply, with the guest root able to change them.
            signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL); signal(SIGPIPE, SIG_DFL);
            clearenv(); setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
            setenv("HOME", home.c_str(), 1); setenv("USER", username.c_str(), 1); setenv("LOGNAME", username.c_str(), 1);
            setenv("SHELL", shell.c_str(), 1); setenv("TERM", "xterm-256color", 1); setenv("COLORTERM", "truecolor", 1);
            setenv("LC_ALL", "C.UTF-8", 1); setenv("PS1", "\\u@goblin:\\w\\$ ", 1);
            // login establishes the normal Debian PAM/logind session, user
            // service manager, shell profile and accounting. Android already
            // authenticated the owner, so this local console uses -f.
            if (!execute && !rescue) { execl("/usr/bin/login", "login", "-f", "-p", username.c_str(), nullptr); perror("login"); _exit(127); }
            if (initgroups(username.c_str(), gid) || setgid(gid) || setuid(uid) || chdir(home.c_str())) { perror("login"); _exit(126); }
            if (execute) execl("/bin/sh", "sh", "-c", payload.c_str(), nullptr);
            else if (rescue) execl("/bin/sh", "sh", "-i", nullptr);
            else execl(shell.c_str(), shell.c_str(), "--noprofile", "-i", nullptr);
            perror("exec shell"); _exit(127);
        }
        fcntl(master, F_SETFD, FD_CLOEXEC); fcntl(master, F_SETFL, O_NONBLOCK);
        terminals[id] = Terminal{pid, master, {}}; int32_t number = pid; Send(Started, id, &number, sizeof(number));
    } catch (const std::exception& error) { Failure(id, error.what()); }
}
void Dispatch(const Header& h, const std::string& data) {
    auto found = terminals.find(h.id);
    if (h.operation == Open || h.operation == Execute) Start(h.id, data, h.operation == Execute);
    else if (h.operation == DnsAnswer && dns) dns->answer(h.id, data);
    else if (h.operation == Shutdown) {
        if (getpid() == 1) stopping = 1;
        else if (!fork()) { execl("/usr/bin/systemctl", "systemctl", "--no-block", "poweroff", nullptr); _exit(127); }
    }
    else if (h.operation == ReadFile) {
        try {
            if (data.find('\0') != std::string::npos) throw std::runtime_error("Invalid file path");
            Fd file(open(data.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)); struct stat st{};
            Require(fstat(file.fd, &st) == 0 && S_ISREG(st.st_mode), "read regular file");
            char bytes[65536]; ssize_t n;
            while ((n = read(file.fd, bytes, sizeof(bytes))) != 0) {
                if (n < 0 && errno == EINTR) continue;
                Require(n > 0, data); Send(FileData, h.id, bytes, n);
            }
            int32_t ok = 0; Send(Exited, h.id, &ok, sizeof(ok));
        } catch (const std::exception& error) { Failure(h.id, error.what()); }
    } else if (found != terminals.end()) {
        auto& terminal = found->second;
        if (h.operation == Input) terminal.input.add(data.data(), data.size());
        else if (h.operation == Resize && data.size() == sizeof(Size)) {
            Size value; memcpy(&value, data.data(), sizeof(value));
            winsize size{uint16_t(value.rows), uint16_t(value.columns), 0, 0}; ioctl(terminal.fd, TIOCSWINSZ, &size);
        } else if (h.operation == Close) {
            close(terminal.fd); terminal.fd = -1; kill(terminal.pid, SIGHUP);
        }
    }
}
void Agent() {
    signal(SIGTERM, Stop); signal(SIGINT, Stop); signal(SIGPIPE, SIG_IGN);
    Fd channel(open("/dev/ttyS0", O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC));
    termios settings{}; Require(tcgetattr(channel.fd, &settings) == 0, "serial attributes");
    cfmakeraw(&settings); settings.c_cflag |= CLOCAL | CREAD; Require(tcsetattr(channel.fd, TCSANOW, &settings) == 0, "raw control channel");
    if (!rescue) dns = std::make_unique<GuestDns>([](uint32_t id, const std::string& bytes) { Send(DnsQuery, id, bytes.data(), bytes.size()); });
    utsname version{}; uname(&version); Send(rescue ? RescueReady : Ready, 0, version.release, strlen(version.release));
    std::string incoming;
    while (!stopping) {
        std::vector<pollfd> descriptors{{channel.fd, short(POLLIN | (!transport.empty() ? POLLOUT : 0)), 0}};
        std::vector<uint32_t> ids;
        for (const auto& item : terminals) if (item.second.fd >= 0) {
            descriptors.push_back({item.second.fd, short(POLLIN | (!item.second.input.empty() ? POLLOUT : 0)), 0}); ids.push_back(item.first);
        }
        size_t dns_start = descriptors.size();
        if (dns) dns->pollfds(descriptors);
        int rc = poll(descriptors.data(), descriptors.size(), 100);
        if (rc < 0 && errno == EINTR) continue;
        Require(rc >= 0, "poll terminals");
        if (dns) dns->dispatch(descriptors, dns_start);
        if (descriptors[0].revents & POLLOUT) transport.flush(channel.fd);
        if (descriptors[0].revents & POLLIN) {
            char bytes[65536]; ssize_t n = read(channel.fd, bytes, sizeof(bytes));
            if (n > 0) incoming.append(bytes, n);
        }
        while (incoming.size() >= sizeof(Header)) {
            Header h; memcpy(&h, incoming.data(), sizeof(h));
            if (h.magic != kMagic) { incoming.erase(0, 1); continue; }
            if (incoming.size() - sizeof(h) < h.size) break;
            std::string data = incoming.substr(sizeof(h), h.size); incoming.erase(0, sizeof(h) + h.size); Dispatch(h, data);
        }
        for (size_t i = 0; i < ids.size(); ++i) {
            auto found = terminals.find(ids[i]); if (found == terminals.end() || found->second.fd < 0) continue;
            auto& terminal = found->second; auto& event = descriptors[i + 1];
            if (event.revents & POLLOUT) {
                try { terminal.input.flush(terminal.fd, true); } catch (...) { close(terminal.fd); terminal.fd = -1; }
            }
            if (terminal.fd >= 0 && event.revents & (POLLIN | POLLHUP)) {
                char bytes[65536]; ssize_t n = read(terminal.fd, bytes, sizeof(bytes));
                if (n > 0) Send(Output, ids[i], bytes, n);
                // login revokes and reopens its slave while establishing a
                // session. A temporary EIO/HUP must not destroy the master.
                // The child reaper below owns terminal lifetime.
            }
        }
        int status; pid_t child;
        while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
            for (auto it = terminals.begin(); it != terminals.end(); ++it) if (it->second.pid == child) {
                char bytes[65536]; ssize_t n;
                while (it->second.fd >= 0 && (n = read(it->second.fd, bytes, sizeof(bytes))) > 0) Send(Output, it->first, bytes, n);
                if (it->second.fd >= 0) close(it->second.fd);
                int32_t code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                Send(Exited, it->first, &code, sizeof(code)); terminals.erase(it); break;
            }
        }
    }
    if (getpid() == 1) { kill(-1, SIGTERM); sync(); reboot(RB_POWER_OFF); }
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--agent") {
        try { Agent(); return 0; }
        catch (const std::exception& error) { fprintf(stderr, "%s\n", error.what()); return 1; }
    }
    Fd original_root(open("/", O_RDONLY | O_DIRECTORY));
    try {
        if (argc == 4 && std::string(argv[1]) == "--unpack") { Unpack(argv[2], argv[3]); return 0; }
        Mount("proc", "/proc", "proc");
        std::string command;
        if (FILE* f = fopen("/proc/cmdline", "r")) { char b[4096]; while (fgets(b, sizeof(b), f)) command += b; fclose(f); }
        if (command.find("goblin.rescue=1") != std::string::npos) throw std::runtime_error("Rescue boot requested");
        Boot();
        // Debian owns PID 1 and normal service management. The APK-owned
        // terminal broker runs as an ordinary service from volatile /run.
        unsetenv("DEBIAN_FRONTEND");
        execl("/usr/lib/systemd/systemd", "systemd", nullptr);
        throw std::runtime_error("Cannot start Debian systemd");
    } catch (const std::exception& error) { fprintf(stderr, "GOBLIN UML ERROR: %s\n", error.what()); fflush(stderr); }
    if (getpid() == 1) {
        try {
            Require(fchdir(original_root.fd) == 0 && chroot(".") == 0 && chdir("/") == 0, "return to rescue root");
            Mount("devtmpfs", "/dev", "devtmpfs"); Directory("/dev/pts");
            Mount("devpts", "/dev/pts", "devpts", 0, "ptmxmode=0666");
            unlink("/dev/ptmx"); symlink("pts/ptmx", "/dev/ptmx");
            Mount("proc", "/proc", "proc"); Mount("sysfs", "/sys", "sysfs");
            rescue = true;
            puts("Rescue shell: the Linux disk is retained at /dev/ubda. Mount it at /mnt to inspect it, or run e2fsck while unmounted.");
            Agent();
        } catch (const std::exception& error) { fprintf(stderr, "Rescue failure: %s\n", error.what()); }
        sync(); reboot(RB_POWER_OFF); for (;;) pause();
    }
    return 1;
}
