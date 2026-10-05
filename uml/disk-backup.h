#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <zlib.h>

namespace goblin_disk {
struct File {
    int fd;
    explicit File(int value) : fd(value) { if (fd < 0) throw std::runtime_error(strerror(errno)); }
    ~File() { close(fd); }
    File(const File&) = delete;
};
inline void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(std::string(message) + ": " + strerror(errno)); }
struct Gzip {
    gzFile value;
    Gzip(int fd, const char* mode) : value(nullptr) {
        int duplicate = dup(fd); require(duplicate >= 0, "Duplicate backup stream");
        value = gzdopen(duplicate, mode);
        if (!value) { close(duplicate); throw std::runtime_error("Cannot open backup stream"); }
    }
    ~Gzip() { if (value) gzclose(value); }
    void write(const void* data, size_t n) { if (gzwrite(value, data, n) != int(n)) throw std::runtime_error("Writing backup failed; check destination space"); }
    void read(void* data, size_t n) { if (gzread(value, data, n) != int(n)) throw std::runtime_error("Backup is truncated or damaged"); }
    void finish() { int result = gzclose(value); value = nullptr; if (result != Z_OK) throw std::runtime_error("Backup stream did not complete"); }
};
struct Header { char magic[8]; uint64_t length; };
struct Extent { uint64_t offset, length; };
static_assert(sizeof(Header) == 16 && sizeof(Extent) == 16, "backup wire layout");
inline void io(int fd, void* data, size_t count, uint64_t at, bool writing) {
    char* bytes = static_cast<char*>(data);
    while (count) {
        ssize_t n = writing ? pwrite(fd, bytes, count, at) : pread(fd, bytes, count, at);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, writing ? "Write restored disk; check phone storage" : "Read environment disk");
        bytes += n; count -= n; at += n;
    }
}
inline void validate(int fd, uint64_t size) {
    unsigned char super[1024];
    io(fd, super, sizeof(super), 1024, false);
    if (super[56] != 0x53 || super[57] != 0xef) throw std::runtime_error("Backup does not contain an ext4 environment disk");
    auto u32 = [&](size_t at) -> uint64_t { return uint64_t(super[at]) | uint64_t(super[at+1])<<8 | uint64_t(super[at+2])<<16 | uint64_t(super[at+3])<<24; };
    uint64_t shift = u32(24), blocks = u32(4);
    if (u32(96) & 0x80) blocks |= u32(0x150) << 32;
    if (shift > 6 || blocks > (size >> (10 + shift))) throw std::runtime_error("Backup ext4 geometry exceeds the saved disk");
}
// Call only while holding debian.lock. A killed import can leave its private
// staging file behind; it was never published as the active or previous disk.
inline void discardInterruptedImports(const std::string& directory) {
    DIR* entries = opendir(directory.c_str()); require(entries != nullptr, "Inspect incomplete restores");
    const std::string prefix = "rootfs.restore.";
    while (dirent* entry = readdir(entries)) {
        std::string name(entry->d_name);
        if (name.compare(0, prefix.size(), prefix) || name.size() != prefix.size() + 6) continue;
        if (!std::all_of(name.begin() + prefix.size(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        })) continue;
        std::string path = directory + "/" + name; struct stat info{};
        if (lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) unlink(path.c_str());
    }
    closedir(entries);
}
inline void exportDisk(const std::string& directory, int destination, const std::function<void(uint64_t)>& progress = {}) {
    File lock(open((directory + "/../debian.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
    require(flock(lock.fd, LOCK_EX | LOCK_NB) == 0, "The environment must be stopped for a consistent backup");
    discardInterruptedImports(directory);
    File disk(open((directory + "/rootfs.ext4").c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat st{}; require(fstat(disk.fd, &st) == 0, "Read disk size"); validate(disk.fd, st.st_size);
    Gzip out(destination, "wb1"); Header header{{'G','B','L','N','D','K','0','1'}, uint64_t(st.st_size)}; out.write(&header, sizeof(header));
    char bytes[65536]; uint64_t at = 0, done = 0; bool sparse = true;
    while (at < header.length) {
        uint64_t end = header.length;
        if (sparse) {
            off_t data = lseek(disk.fd, at, SEEK_DATA);
            if (data < 0 && errno == ENXIO) break;
            if (data < 0 && (errno == EINVAL || errno == ENOTSUP)) sparse = false;
            else {
                require(data >= 0, "Find allocated disk data"); at = data;
                off_t hole = lseek(disk.fd, data, SEEK_HOLE); require(hole > data, "Find sparse disk hole");
                end = std::min<uint64_t>(hole, header.length);
            }
        }
        while (at < end) {
            size_t count = std::min<uint64_t>(sizeof(bytes), end - at);
            io(disk.fd, bytes, count, at, false);
            if (!std::all_of(bytes, bytes + count, [](char b) { return b == 0; })) {
                Extent extent{at, count}; out.write(&extent, sizeof(extent)); out.write(bytes, count);
            }
            at += count; done += count; if (progress) progress(done);
        }
    }
    Extent end{UINT64_MAX, 0}; out.write(&end, sizeof(end)); out.finish();
    // Document providers may give us a pipe rather than a seekable file.
    if (fsync(destination) < 0 && errno != EINVAL && errno != EROFS) require(false, "Sync backup");
}
inline std::string publish(const std::string& directory, const std::string& temporary) {
    const std::string image = directory + "/rootfs.ext4";
    std::string previous; struct stat old{};
    if (lstat(image.c_str(), &old) == 0) {
        if (!S_ISREG(old.st_mode)) throw std::runtime_error("Existing disk is not a regular file");
        previous = directory + "/rootfs.before-restore.XXXXXX";
        File reservation(mkstemp(previous.data()));
    } else require(errno == ENOENT, "Read existing disk");
    File parent(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (previous.empty()) require(rename(temporary.c_str(), image.c_str()) == 0, "Publish restored disk");
    else {
        // Android's app SELinux domain forbids hard links on some releases.
        // Exchange the two directory entries atomically instead: there is
        // never a point at which the active disk is absent, and the displaced
        // disk is already at its final recovery path when the swap commits.
        try {
            require(rename(temporary.c_str(), previous.c_str()) == 0, "Stage restored disk");
            require(fsync(parent.fd) == 0, "Commit incoming recovery disk");
            require(syscall(SYS_renameat2, AT_FDCWD, previous.c_str(), AT_FDCWD, image.c_str(), 2 /* RENAME_EXCHANGE */) == 0, "Exchange environment disks atomically");
        } catch (...) { unlink(previous.c_str()); throw; }
    }
    require(fsync(parent.fd) == 0, "Commit restored disk");
    return previous;
}
inline std::string importDisk(const std::string& directory, int source, const std::function<void(uint64_t)>& progress = {}) {
    File lock(open((directory + "/../debian.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
    require(flock(lock.fd, LOCK_EX | LOCK_NB) == 0, "The environment must be stopped to restore a backup");
    discardInterruptedImports(directory);
    std::string temporary = directory + "/rootfs.restore.XXXXXX";
    File disk(mkstemp(temporary.data()));
    try {
        Gzip input(source, "rb"); Header header{}; input.read(&header, sizeof(header));
        if (memcmp(header.magic, "GBLNDK01", 8) || header.length < 2048 || header.length > uint64_t(std::numeric_limits<off_t>::max()))
            throw std::runtime_error("Not a supported Goblin disk backup");
        require(ftruncate(disk.fd, header.length) == 0, "Size restored disk");
        uint64_t at = 0, done = 0; char bytes[65536];
        for (;;) {
            Extent extent{}; input.read(&extent, sizeof(extent));
            if (extent.offset == UINT64_MAX && !extent.length) break;
            if (!extent.length || extent.offset < at || extent.offset > header.length || extent.length > header.length - extent.offset)
                throw std::runtime_error("Backup has invalid or overlapping disk extents");
            at = extent.offset;
            for (uint64_t left = extent.length; left;) {
                size_t n = std::min<uint64_t>(left, sizeof(bytes)); input.read(bytes, n);
                io(disk.fd, bytes, n, at, true);
                at += n; left -= n; done += n; if (progress) progress(done);
            }
        }
        int extra = gzread(input.value, bytes, 1);
        if (extra != 0 || !gzeof(input.value)) throw std::runtime_error("Backup checksum failed or unexpected trailing data found");
        input.finish(); validate(disk.fd, header.length);
        require(fsync(disk.fd) == 0, "Sync restored disk");
        return publish(directory, temporary);
    } catch (...) { unlink(temporary.c_str()); throw; }
}
inline std::string restorePrevious(const std::string& directory, const std::string& name, const std::function<void(uint64_t)>& progress = {}) {
    if (name.find('/') != std::string::npos || name.compare(0, 22, "rootfs.before-restore.") != 0)
        throw std::runtime_error("Invalid previous disk name");
    File lock(open((directory + "/../debian.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
    require(flock(lock.fd, LOCK_EX | LOCK_NB) == 0, "The environment must be stopped to restore a previous disk");
    discardInterruptedImports(directory);
    std::string previous = directory + "/" + name, image = directory + "/rootfs.ext4";
    File source(open(previous.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat st{}; require(fstat(source.fd, &st) == 0 && S_ISREG(st.st_mode), "Read previous disk");
    validate(source.fd, st.st_size);
    require(lstat(image.c_str(), &st) == 0 && S_ISREG(st.st_mode), "Read active disk");
    File parent(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    // Both disks already exist independently. Swapping them needs no third
    // full-sized copy and preserves the replaced disk at the selected path.
    require(syscall(SYS_renameat2, AT_FDCWD, previous.c_str(), AT_FDCWD, image.c_str(), 2 /* RENAME_EXCHANGE */) == 0,
            "Exchange previous environment disk atomically");
    require(fsync(parent.fd) == 0, "Commit previous disk restore");
    if (progress) progress(0);
    return previous;
}
}
