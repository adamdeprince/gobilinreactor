#include "vfs_tests.h"
#include "vfs.h"
#include "elf_loader.h"
#include <cerrno>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

bool RunVfsTests(const std::string& root, const std::function<void(const std::string&)>& log) {
    bool ok = true; unsigned checks = 0;
    auto check = [&](bool value, const char* name) {
        ++checks; if (!value) { ok = false; log(std::string("FAILED VFS/ELF: ") + name); }
    };
    goblin::Vfs vfs; std::string error;
    check(vfs.Mount(root, &error), "mount fixture root");
    auto open_check = [&](const char* path, int expected) {
        int fd = vfs.Open(path, O_RDONLY);
        check(expected == 0 ? fd >= 0 : fd == expected, path);
        if (fd >= 0) close(fd);
    };
    open_check("/bin/true", 0);
    open_check("/../../../../etc/hostname", 0);
    const std::string sentinel = root + "-outside";
    int outside = open(sentinel.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    check(outside >= 0, "create outside-root sentinel");
    if (outside >= 0) { (void)!write(outside, "must not escape", 15); close(outside); }
    check(symlink(sentinel.c_str(), (root + "/tmp/escape").c_str()) == 0, "outside absolute symlink");
    open_check("/tmp/escape", -ENOENT);
    check(symlink("../../../../proc/self/maps", (root + "/tmp/proc-escape").c_str()) == 0, "proc escape symlink");
    open_check("/tmp/proc-escape", -ENOENT);
    check(symlink("/etc", (root + "/tmp/etc-link").c_str()) == 0, "absolute guest symlink");
    open_check("/tmp/etc-link/hostname", 0);
    open_check("/tmp/etc-link/../bin/true", 0);
    check(symlink("loop", (root + "/tmp/loop").c_str()) == 0, "cyclic symlink");
    open_check("/tmp/loop", -ELOOP);
    open_check("/etc/hostname/child", -ENOTDIR);
    int fd = vfs.Open("/tmp/escape", O_RDONLY | O_NOFOLLOW);
    check(fd == -ELOOP, "O_NOFOLLOW rejects final symlink");
    if (fd >= 0) close(fd);
    fd = vfs.Open("/tmp/escape", O_WRONLY | O_CREAT | O_EXCL, 0600);
    check(fd == -EEXIST, "exclusive creation rejects dangling symlink");
    if (fd >= 0) close(fd);
    check(unlink(sentinel.c_str()) == 0, "remove sentinel");

    int original = vfs.Open("/tmp/inode-a", O_RDWR | O_CREAT | O_EXCL, 0600);
    check(original >= 0, "create hard-link source");
    if (original >= 0) {
        check(write(original, "before", 6) == 6 && vfs.Link("/tmp/inode-a", "/tmp/inode-b", false) == 0,
              "hard links work without host link permission");
        struct stat a{}, b{};
        check(vfs.Stat("/tmp/inode-a", &a, false) == 0 && vfs.Stat("/tmp/inode-b", &b, false) == 0 &&
              S_ISREG(a.st_mode) && a.st_ino == b.st_ino && a.st_nlink == 2 && b.st_nlink == 2,
              "hard-link identities and counts");
        std::string target;
        check(vfs.Readlink("/tmp/inode-b", &target) == -EINVAL, "hard links are not guest symlinks");
        int alias = vfs.Open("/tmp/inode-b", O_RDWR | O_NOFOLLOW);
        char bytes[6]{};
        check(alias >= 0 && pwrite(alias, "after!", 6, 0) == 6 && pread(original, bytes, 6, 0) == 6 &&
              !memcmp(bytes, "after!", 6), "hard-link writes reach already-open source descriptor");
        if (alias >= 0) close(alias);
        goblin::Vfs reopened;
        check(reopened.Mount(root, &error) && reopened.Stat("/tmp/inode-b", &b, false) == 0 && b.st_nlink == 2,
              "hard-link metadata survives remount");
        check(vfs.Rename("/tmp/inode-a", "/tmp/inode-b") == 0 && vfs.Stat("/tmp/inode-a", &a) == 0,
              "renaming aliases of one inode is a no-op");
        check(vfs.Unlink("/tmp/inode-a", false) == 0 && vfs.Stat("/tmp/inode-b", &b) == 0 && b.st_nlink == 1,
              "unlinking original preserves other hard link");
        check(vfs.Unlink("/tmp/inode-b", false) == 0 && vfs.StatFd(original, &a) == 0 && a.st_nlink == 0,
              "last unlink preserves open descriptor with zero links");
        check(vfs.Open("/.goblin-inodes", O_RDONLY) == -ENOENT &&
              vfs.Symlink("goblin-inode:1", "/tmp/forged-inode") == -EPERM,
              "inode metadata cannot be addressed or forged by guest paths");
        close(original);
    }

    check(vfs.Mkdir("/tmp/mode-zero", 0) == 0, "create mode-zero directory");
    int zero = vfs.Open("/tmp/mode-zero/file", O_CREAT | O_RDWR, 0);
    struct stat permissions{};
    check(zero >= 0 && vfs.StatFd(zero, &permissions) == 0 && (permissions.st_mode & 07777) == 0 &&
          write(zero, "root", 4) == 4, "virtual root accesses mode-zero paths without changing guest permissions");
    check(zero >= 0 && vfs.ChownFd(zero, 123, 456) == 0 && vfs.ChmodFd(zero, 04750) == 0 &&
          vfs.StatFd(zero, &permissions) == 0 && permissions.st_uid == 123 && permissions.st_gid == 456 &&
          (permissions.st_mode & 07777) == 04750, "inode ownership and complete permission bits");
    if (zero >= 0) close(zero);
    goblin::Vfs permission_remount;
    check(permission_remount.Mount(root, &error) && permission_remount.Stat("/tmp/mode-zero/file", &permissions) == 0 &&
          permissions.st_uid == 123 && (permissions.st_mode & 07777) == 04750, "permissions survive remount");
    int directory = vfs.Open("/tmp/mode-zero", O_PATH | O_DIRECTORY);
    std::string moved;
    check(directory >= 0 && vfs.Rename("/tmp/mode-zero", "/tmp/mode-moved") == 0 &&
          vfs.PathFd(directory, &moved) == 0 && moved == "/tmp/mode-moved", "directory descriptors follow rename");
    if (directory >= 0) close(directory);

    // Kill a separate broker at durable boundaries, then mount from scratch.
    // These test process interruption, not the storage hardware's power-loss guarantees.
    for (const char* stage : {"promotion-journal", "promotion-moved", "promotion-source", "link-entry", "unlink-entry", "rename-entry"}) {
        const std::string source = std::string("/tmp/crash-") + stage, target = source + "-alias", replacement = source + "-replacement";
        int fd = vfs.Open(source, O_CREAT | O_EXCL | O_RDWR, 0600);
        bool setup = fd >= 0 && write(fd, "recover", 7) == 7 && fsync(fd) == 0;
        if (fd >= 0) close(fd);
        const bool unlinking = !strcmp(stage, "unlink-entry"), renaming = !strcmp(stage, "rename-entry");
        if (unlinking || renaming) setup = vfs.Link(source, target, false) == 0 && setup;
        if (renaming) { fd = vfs.Open(replacement, O_CREAT | O_RDWR, 0600); setup = fd >= 0 && setup; if (fd >= 0) close(fd); }
        pid_t child = setup ? fork() : -1;
        if (child == 0) {
            vfs.transaction_hook = [stage](const char* point) { if (!strcmp(stage, point)) _exit(73); };
            if (unlinking) vfs.Unlink(source, false);
            else if (renaming) vfs.Rename(replacement, source);
            else vfs.Link(source, target, false);
            _exit(74);
        }
        int status = 0;
        if (child > 0) while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        goblin::Vfs recovered;
        bool good = child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 73 && recovered.Mount(root, &error);
        struct stat a{}, b{};
        const bool linked = !strcmp(stage, "link-entry");
        good = good && recovered.Stat(unlinking || renaming ? target : source, &a) == 0 && a.st_nlink == (linked ? 2 : 1);
        std::vector<uint8_t> bytes;
        good = good && recovered.ReadFile(unlinking || renaming ? target : source, &bytes, &error) && std::string(bytes.begin(), bytes.end()) == "recover";
        if (linked) good = good && recovered.Stat(target, &b) == 0 && a.st_ino == b.st_ino;
        check(good, stage);
        check(vfs.Mount(root, &error), "original broker observes recovered metadata");
    }

    std::vector<uint8_t> executable;
    check(vfs.ReadFile("/bin/true", &executable, &error), "read unmodified Debian ELF");
    if (executable.size() >= sizeof(Elf64_Ehdr)) {
        Elf64_Ehdr header; memcpy(&header, executable.data(), sizeof(header));
        auto bad = executable;
        header.e_phoff = UINT64_MAX - 7; memcpy(bad.data(), &header, sizeof(header));
        goblin::LoadedImage image;
        check(!goblin::LoadElf(executable.data(), executable.size(), &image, &error, 0, 1024), "ELF memory budget rejects before mapping");
        check(!goblin::LoadElf(bad.data(), bad.size(), &image, &error), "program-header offset overflow");
        memcpy(&header, executable.data(), sizeof(header));
        bad = executable;
        for (unsigned i = 0; i < header.e_phnum; ++i) {
            Elf64_Phdr ph; size_t offset = header.e_phoff + i * sizeof(ph);
            memcpy(&ph, bad.data() + offset, sizeof(ph));
            if (ph.p_type != PT_LOAD) continue;
            ph.p_vaddr = UINT64_MAX - 4095; ph.p_memsz = 8192;
            memcpy(bad.data() + offset, &ph, sizeof(ph)); break;
        }
        check(!goblin::LoadElf(bad.data(), bad.size(), &image, &error), "segment virtual-address overflow");
        bad = executable;
        for (unsigned i = 0; i < header.e_phnum; ++i) {
            Elf64_Phdr ph; size_t offset = header.e_phoff + i * sizeof(ph);
            memcpy(&ph, bad.data() + offset, sizeof(ph));
            if (ph.p_type == PT_INTERP) { bad[ph.p_offset + ph.p_filesz - 1] = 'x'; break; }
        }
        check(!goblin::LoadElf(bad.data(), bad.size(), &image, &error), "unterminated ELF interpreter");
    }
    log("VFS/ELF regressions  " + std::to_string(checks) + (ok ? " checks passed" : " checks, failures above"));
    return ok;
}
