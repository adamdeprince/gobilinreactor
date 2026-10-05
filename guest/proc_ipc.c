#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c, n) do { if (!(c)) { printf("proc/IPC check %d failed: errno=%d\n", n, errno); return n; } } while (0)
static int text(const char* path, char* bytes, size_t size) {
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    int n = read(fd, bytes, size - 1); close(fd); if (n >= 0) bytes[n] = 0; return n;
}
static int sendfd(int socket, int fd) {
    char byte = 'R', control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {&byte, 1};
    struct msghdr message = {.msg_iov=&iov, .msg_iovlen=1, .msg_control=control, .msg_controllen=sizeof(control)};
    struct cmsghdr* c = CMSG_FIRSTHDR(&message); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(fd)); return sendmsg(socket, &message, MSG_NOSIGNAL);
}
static int recvfd(int socket) {
    char byte, control[CMSG_SPACE(sizeof(int))] = {0}; struct iovec iov = {&byte, 1};
    struct msghdr message = {.msg_iov=&iov, .msg_iovlen=1, .msg_control=control, .msg_controllen=sizeof(control)};
    if (recvmsg(socket, &message, MSG_CMSG_CLOEXEC) != 1 || byte != 'R' || message.msg_flags) return -1;
    struct cmsghdr* c = CMSG_FIRSTHDR(&message); int fd = -1;
    if (!c || c->cmsg_type != SCM_RIGHTS || c->cmsg_level != SOL_SOCKET || c->cmsg_len != CMSG_LEN(sizeof(fd))) return -1;
    memcpy(&fd, CMSG_DATA(c), sizeof(fd)); return fd;
}
int main(void) {
    char data[32768], path[256], value[128];
    pid_t self = getpid();
    int n = readlink("/proc/self", value, sizeof(value)-1); CHECK(n > 0, 1); value[n] = 0; CHECK(atoi(value) == self, 2);
    n = readlink("/proc/thread-self", value, sizeof(value)-1); CHECK(n > 0, 3); value[n] = 0;
    snprintf(path, sizeof(path), "%d/task/%ld", self, syscall(SYS_gettid)); CHECK(!strcmp(value, path), 4);
    CHECK(text("/proc/self/status", data, sizeof(data)) > 0, 5);
    snprintf(path, sizeof(path), "Pid:\t%d\n", self); CHECK(strstr(data, path) && strstr(data, "Uid:\t0\t0\t0\t0"), 6);
    CHECK(text("/proc/self/maps", data, sizeof(data)) > 0 && !strstr(data, "/apex/") && !strstr(data, "/data/app/"), 7);
    int contains = 0;
    for (char* line = data; line && *line;) {
        unsigned long start, end; if (sscanf(line, "%lx-%lx", &start, &end) == 2 && (unsigned long)&main >= start && (unsigned long)&main < end) contains = 1;
        char* newline = strchr(line, '\n'); line = newline ? newline + 1 : NULL;
    }
    CHECK(contains, 8);
    DIR* directory = opendir("/proc"); CHECK(directory, 9);
    int processes = 0; struct dirent* entry;
    while ((entry = readdir(directory))) if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') { CHECK(atoi(entry->d_name) == self, 10); ++processes; }
    CHECK(processes == 1, 11); closedir(directory);
    unlink("/tmp/proc-alias"); CHECK(!symlink("/proc", "/tmp/proc-alias"), 12);
    CHECK(text("/tmp/proc-alias/self/status", data, sizeof(data)) > 0, 13);
    CHECK(open("/tmp/proc-alias/self/status", O_WRONLY) == -1, 14);
    CHECK(open("/proc/self/mem", O_RDONLY) == -1 && errno == ENOENT, 15);
    CHECK(text("/proc/../etc/passwd", data, sizeof(data)) > 0, 16);
    int fd = open("/tmp/proc-fd-data", O_CREAT | O_RDWR | O_TRUNC, 0600); CHECK(fd >= 0, 17);
    CHECK(write(fd, "abcdef", 6) == 6 && lseek(fd, 0, SEEK_SET) == 0, 18);
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    n = readlink(path, value, sizeof(value)-1); CHECK(n > 0, 19); value[n] = 0; CHECK(!strcmp(value, "/tmp/proc-fd-data"), 20);
    int reopened = open(path, O_RDONLY); CHECK(reopened >= 0 && read(reopened, data, 1) == 1 && data[0] == 'a' && lseek(fd, 0, SEEK_CUR) == 0, 21); close(reopened);
    struct stat mode;
    CHECK(!chmod(path, 0640) && !fstat(fd, &mode) && (mode.st_mode & 0777) == 0640, 62);
    CHECK(!fchmodat(AT_FDCWD, "/tmp/proc-fd-data", 0600, AT_SYMLINK_NOFOLLOW) &&
          !fstat(fd, &mode) && (mode.st_mode & 0777) == 0600, 63);
    CHECK(!unlink("/tmp/proc-fd-data"), 22);
    CHECK(!chmod(path, 0640) && !fstat(fd, &mode) && (mode.st_mode & 0777) == 0640, 64);
    CHECK(chmod("/proc/self/status", 0600) == -1 && errno == EROFS, 65);
    reopened = open(path, O_RDONLY); CHECK(reopened >= 0 && read(reopened, data, 6) == 6 && !memcmp(data, "abcdef", 6), 23); close(reopened);
    int pair[2]; CHECK(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 24);
    CHECK(fcntl(pair[0], F_GETFD) == FD_CLOEXEC, 25);
    struct ucred cred = {0}; socklen_t length = sizeof(cred);
    CHECK(!getsockopt(pair[0], SOL_SOCKET, SO_PEERCRED, &cred, &length) && cred.pid == self && !cred.uid && !cred.gid, 26);
    CHECK(sendfd(pair[0], 987654) == -1 && errno == EBADF, 27);
    CHECK(sendfd(pair[0], fd) == 1, 28);
    int received = recvfd(pair[1]); CHECK(received >= 0 && fcntl(received, F_GETFD) == FD_CLOEXEC, 29);
    CHECK(read(received, data, 2) == 2 && !memcmp(data, "ab", 2) && lseek(fd, 0, SEEK_CUR) == 2, 30);
    close(received);
    int child = fork(); CHECK(child >= 0, 31);
    if (!child) {
        close(pair[0]); close(fd);
        int moved = recvfd(pair[1]);
        if (moved < 0 || read(moved, data, 4) != 4 || memcmp(data, "cdef", 4)) _exit(71);
        if (write(pair[1], "ok", 2) != 2) _exit(72);
        close(moved); close(pair[1]); _exit(0);
    }
    close(pair[1]);
    snprintf(path, sizeof(path), "/proc/%d/status", child); CHECK(text(path, data, sizeof(data)) > 0, 32);
    snprintf(value, sizeof(value), "PPid:\t%d\n", self); CHECK(strstr(data, value), 33);
    CHECK(sendfd(pair[0], fd) == 1, 34); close(fd);
    CHECK(read(pair[0], data, 2) == 2 && !memcmp(data, "ok", 2), 35);
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), 36);
    CHECK(text(path, data, sizeof(data)) == -1 && errno == ENOENT, 37); close(pair[0]);
    struct sockaddr_un address = {.sun_family = AF_UNIX}; strcpy(address.sun_path, "/tmp/goblin-guest.sock"); unlink(address.sun_path);
    int server = socket(AF_UNIX, SOCK_STREAM, 0), client = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(server >= 0 && client >= 0, 38);
    CHECK(!bind(server, (struct sockaddr*)&address, sizeof(address)) && !listen(server, 4), 39);
    struct stat st; CHECK(!stat(address.sun_path, &st) && S_ISSOCK(st.st_mode), 40);
    CHECK(!connect(client, (struct sockaddr*)&address, sizeof(address)), 41);
    int accepted = accept4(server, 0, 0, SOCK_CLOEXEC | SOCK_NONBLOCK); CHECK(accepted >= 0, 42);
    CHECK(read(accepted, data, 1) == -1 && errno == EAGAIN, 43);
    struct pollfd ready = {.fd = accepted, .events = POLLIN}; CHECK(poll(&ready, 1, 0) == 0, 44);
    CHECK(write(client, "local", 5) == 5 && poll(&ready, 1, 0) == 1 && (ready.revents & POLLIN), 45);
    CHECK(read(accepted, data, sizeof(data)) == 5 && !memcmp(data, "local", 5), 46);
    CHECK(!shutdown(client, SHUT_WR) && read(accepted, data, 1) == 0, 47);
    close(accepted); close(client); close(server);
    client = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(connect(client, (struct sockaddr*)&address, sizeof(address)) == -1 && errno == ECONNREFUSED, 48); close(client);
    CHECK(!unlink(address.sun_path), 49);
    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair), 50);
    CHECK(send(pair[0], "packet", 6, 0) == 6 && recv(pair[1], data, 3, MSG_PEEK | MSG_TRUNC) == 6 && !memcmp(data, "pac", 3), 51);
    CHECK(recv(pair[1], data, sizeof(data), 0) == 6 && !memcmp(data, "packet", 6), 52); close(pair[0]); close(pair[1]);
    server = socket(AF_UNIX, SOCK_DGRAM, 0); client = socket(AF_UNIX, SOCK_DGRAM, 0);
    memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX; memcpy(address.sun_path + 1, "goblin-private", 14);
    CHECK(!bind(server, (struct sockaddr*)&address, 17), 53);
    CHECK(sendto(client, "abstract", 8, 0, (struct sockaddr*)&address, 17) == 8 && recv(server, data, sizeof(data), 0) == 8 && !memcmp(data, "abstract", 8), 54);
    close(client); close(server);
    for (int i = 0; i < 128; ++i) {
        CHECK(!socketpair(AF_UNIX, SOCK_DGRAM, 0, pair), 55);
        CHECK(sendfd(pair[0], pair[1]) == 1 && sendfd(pair[1], pair[0]) == 1, 56);
        close(pair[0]); close(pair[1]);
    }
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair), 57);
    size_t fill_size = (256u << 10) - 456;
    char* fill = calloc(1, fill_size); CHECK(fill, 58);
    CHECK(send(pair[0], fill, fill_size, 0) == (ssize_t)fill_size, 59);
    ready = (struct pollfd){.fd=pair[0], .events=POLLOUT};
    CHECK(poll(&ready, 1, 0) == 0 && send(pair[0], "x", 1, MSG_DONTWAIT) == -1 && errno == EAGAIN, 60);
    CHECK(recv(pair[1], fill, fill_size, 0) == (ssize_t)fill_size && poll(&ready, 1, 0) == 1 && (ready.revents & POLLOUT), 61);
    free(fill); close(pair[0]); close(pair[1]);
    unlink("/tmp/proc-alias");
    puts("guest procfs, Unix sockets and descriptor passing ok"); return 0;
}
