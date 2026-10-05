/* Exercise real descriptor tables with gaps and several getdents batches. */
#define _GNU_SOURCE
#include "../uml/fd-cleanup.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>

static void check(int cloexec)
{
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct rlimit limit;
        assert(!getrlimit(RLIMIT_NOFILE, &limit));
        limit.rlim_cur = limit.rlim_max;
        assert(!setrlimit(RLIMIT_NOFILE, &limit));
        int base = open("/dev/null", O_RDONLY);
        assert(base >= 0);
        assert(dup2(base, 10) == 10);
        int descriptors[600];
        for (int i = 0; i < 600; ++i) {
            descriptors[i] = fcntl(base, F_DUPFD, 20 + i * 2);
            assert(descriptors[i] >= 0);
        }
        int high = fcntl(base, F_DUPFD, 16385);
        assert(high >= 16385);
        assert(!goblin_cleanup_fds(11, cloexec));
        assert(fcntl(10, F_GETFD) == 0);
        for (int i = 0; i <= 600; ++i) {
            int flags = fcntl(i == 600 ? high : descriptors[i], F_GETFD);
            if (cloexec) assert(flags >= 0 && (flags & FD_CLOEXEC));
            else assert(flags == -1 && errno == EBADF);
        }
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
    puts(cloexec ? "PASS: CLOEXEC covers sparse and high descriptors across directory batches"
                 : "PASS: close covers sparse and high descriptors while preserving protected descriptors");
}

int main(void) { check(0); check(1); return 0; }
