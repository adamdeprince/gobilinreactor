/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Android 12's inherited seccomp filter kills close_range callers rather
 * than returning ENOSYS. Enumerate actual descriptors with post-fork-safe
 * syscalls; neither impose a descriptor cap nor scan the whole RLIMIT. */
static int goblin_cleanup_fds(int first, int cloexec)
{
    struct goblin_dirent64 {
        unsigned long long ino;
        long long offset;
        unsigned short size;
        unsigned char type;
        char name[1];
    };
    union { long long align; char bytes[4096]; } buffer;
    int directory = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return -1;
    for (;;) {
        long length = syscall(__NR_getdents64, directory, buffer.bytes, sizeof(buffer.bytes));
        if (length < 0 && errno == EINTR) continue;
        if (length < 0) { close(directory); return -1; }
        if (!length) break;
        for (long position = 0; position < length;) {
            struct goblin_dirent64 *entry = (struct goblin_dirent64 *)(buffer.bytes + position);
            if (entry->size < offsetof(struct goblin_dirent64, name) + 2 || position + entry->size > length) {
                close(directory); errno = EIO; return -1;
            }
            int fd = 0;
            const char *name = entry->name;
            if (*name >= '0' && *name <= '9') {
                while (*name >= '0' && *name <= '9') fd = fd * 10 + (*name++ - '0');
                if (!*name && fd >= first && fd != directory) {
                    if (cloexec) {
                        int result;
                        do { result = fcntl(fd, F_SETFD, FD_CLOEXEC); } while (result < 0 && errno == EINTR);
                        if (result < 0 && errno != EBADF) { close(directory); return -1; }
                    } else close(fd);
                }
            }
            position += entry->size;
        }
    }
    close(directory);
    return 0;
}
