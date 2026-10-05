#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
static volatile sig_atomic_t pipes;
static void pipe_signal(int number) { if (++pipes > 2) _exit(22); }

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "job")) { puts("JOBREADY"); fflush(stdout); for (;;) pause(); }
    if (!strcmp(argv[1], "sigpipe")) {
        struct sigaction action = {0}; action.sa_handler = pipe_signal; action.sa_flags = SA_RESTART;
        sigemptyset(&action.sa_mask); if (sigaction(SIGPIPE, &action, 0)) return 20;
        int pipefd[2]; if (pipe(pipefd)) return 21; close(pipefd[0]);
        int result = write(pipefd[1], "x", 1); close(pipefd[1]);
        if (result != -1 || errno != EPIPE || pipes != 1) return 23;
        puts("SIGPIPE delivered once"); return 0;
    }
    if (!strcmp(argv[1], "filesystem")) {
        if (mkdir("/tmp/dir-before///", 0700)) return 26;
        if (chdir("/tmp/dir-before")) return 11;
        int directory = open(".", O_RDONLY | O_DIRECTORY);
        if (directory < 0 || rename("/tmp/dir-before", "/tmp/dir-after")) return 12;
        char cwd[128];
        if (!getcwd(cwd, sizeof(cwd)) || strcmp(cwd, "/tmp/dir-after")) return 13;
        int fd = openat(directory, "file", O_CREAT | O_RDWR, 0);
        if (fd < 0 || write(fd, "data", 4) != 4 || chdir("/") || fchdir(directory)) return 14;
        struct stat st;
        if (fstat(fd, &st) || (st.st_mode & 0777) || fchown(fd, 123, 456) || fstat(fd, &st) || st.st_uid != 123 || st.st_gid != 456) return 15;
        int second = open("file", O_RDONLY);
        if (ftruncate(fd, -1) != -1 || errno != EINVAL) return 18;
        if (second < 0 || flock(fd, LOCK_EX) || flock(second, LOCK_EX | LOCK_NB) != -1 || errno != EWOULDBLOCK) return 16;
        if (flock(fd, LOCK_UN) || flock(second, LOCK_EX | LOCK_NB)) return 17;
        close(second); close(fd); unlink("file"); close(directory); chdir("/"); rmdir("/tmp/dir-after");
        puts("filesystem semantics verified"); return 0;
    }
    if (!strcmp(argv[1], "memory")) {
        void* area = mmap(0, 128u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (area != MAP_FAILED || errno != ENOMEM) return 3;
        area = mmap(0, 2u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (area == MAP_FAILED) return 24;
        *(char*)area = 42;
        if (mremap(area, 2u << 20, 128u << 20, MREMAP_MAYMOVE) != MAP_FAILED || errno != ENOMEM || *(char*)area != 42) return 25;
        munmap(area, 2u << 20);
        puts("memory bounded"); return 0;
    }
    if (!strcmp(argv[1], "tasks")) {
        pid_t child = fork(); if (child < 0) return 4;
        if (!child) { for (;;) pause(); }
        pid_t excess = fork();
        int good = excess < 0 && errno == EAGAIN;
        if (!excess) _exit(5);
        if (excess > 0) { kill(excess, SIGKILL); waitpid(excess, 0, 0); }
        kill(child, SIGKILL); waitpid(child, 0, 0);
        if (!good) return 6;
        puts("tasks bounded"); return 0;
    }
    if (!strcmp(argv[1], "disk")) {
        int fd = open("/tmp/quota-file", O_CREAT | O_TRUNC | O_RDWR, 0600);
        if (fd < 0) return 7;
        struct statfs before, path, grown, shrunk, unlinked, released;
        if (fstatfs(fd, &before) || statfs("/tmp", &path)) return 27;
        unsigned long unit = before.f_frsize ? before.f_frsize : before.f_bsize;
        if (!unit || !before.f_bavail || before.f_bavail * unit > (64u << 10) ||
            path.f_blocks != before.f_blocks || path.f_bavail != before.f_bavail) return 28;
        if (ftruncate(fd, 128u << 10) != -1 || errno != ENOSPC) return 8;
        char bytes[128u << 10]; memset(bytes, 'q', sizeof(bytes));
        if (write(fd, bytes, sizeof(bytes)) != -1 || errno != ENOSPC) return 9;
        if (write(fd, bytes, 8192) != 8192 || fstatfs(fd, &grown) ||
            (before.f_bavail - grown.f_bavail) * unit != 8192) return 29;
        if (ftruncate(fd, 0) || fstatfs(fd, &shrunk) || shrunk.f_bavail != before.f_bavail) return 30;
        if (ftruncate(fd, 8192) || unlink("/tmp/quota-file") || fstatfs(fd, &unlinked) ||
            unlinked.f_bavail != grown.f_bavail) return 31;
        close(fd);
        if (statfs("/tmp", &released) || released.f_bavail <= unlinked.f_bavail) return 32;
        puts("disk bounded"); return 0;
    }
    if (!strcmp(argv[1], "spin")) { for (;;) __asm__ volatile("" ::: "memory"); }
    return 10;
}
