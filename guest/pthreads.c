#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <spawn.h>
#include <string.h>

#define CHECK(c, n) do { if (!(c)) { printf("pthread check %d failed\n", n); return n; } } while (0)
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t robust;
static int ready, total, shared_fd;
static atomic_int spinning, stop, caught;
static _Thread_local int tls;
static void handler(int sig) { if (tls == 47) atomic_store(&caught, sig); }
static void* worker(void* input) {
    tls = (int)(intptr_t)input;
    if (getpid() != 1 || syscall(SYS_gettid) == getpid()) return (void*)1;
    pthread_mutex_lock(&mutex);
    ++ready; pthread_cond_broadcast(&cond);
    while (ready < 2) pthread_cond_wait(&cond, &mutex);
    pthread_mutex_unlock(&mutex);
    for (int i = 0; i < 1000; ++i) {
        pthread_mutex_lock(&mutex); ++total; pthread_mutex_unlock(&mutex);
    }
    return (void*)(intptr_t)tls;
}
static void* abandon(void* unused) {
    if (pthread_mutex_lock(&robust)) return (void*)1;
    return 0;
}
static void* spin(void* unused) {
    tls = 47;
    close(shared_fd);
    atomic_store(&spinning, 1);
    while (!atomic_load(&stop)) {}
    return (void*)(intptr_t)(tls != 47);
}
extern char** environ;
static void* spawn_child(void* unused) {
    pid_t pid;
    char* argv[] = {"/bin/true", 0};
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) || posix_spawn_file_actions_addclosefrom_np(&actions, 3)) return (void*)2;
    int rc = posix_spawn(&pid, argv[0], &actions, 0, argv, environ), status;
    posix_spawn_file_actions_destroy(&actions);
    if (rc || waitpid(pid, &status, 0) != pid || status) return (void*)1;
    return 0;
}
static void* unshare_files(void* input) {
    int fd = (int)(intptr_t)input;
    if (close_range(fd, ~0u, 4) || fcntl(fd, F_GETFD) != FD_CLOEXEC) return (void*)1;
    if (close_range(fd, ~0u, 2) || fcntl(fd, F_GETFD) != -1 || errno != EBADF) return (void*)2;
    return 0;
}
static void* exec_child(void* unused) {
    execl("/bin/echo", "echo", "thread exec ok", (char*)0);
    _exit(99);
}
static void* after_leader(void* unused) {
    struct timespec pause = {0, 20000000}; nanosleep(&pause, 0);
    if (unused) { write(1, "group exit ok\n", 14); _exit(19); }
    write(1, "leader exit ok\n", 15);
    return 0;
}
int main(int argc, char** argv) {
    if (argc > 1) {
        pthread_t thread;
        if (!strcmp(argv[1], "exec")) {
            CHECK(!pthread_create(&thread, 0, exec_child, 0), 23);
            for (;;) {}
        }
        CHECK(!pthread_create(&thread, 0, after_leader, (void*)(intptr_t)!strcmp(argv[1], "group-exit")), 24);
        syscall(SYS_exit, 37);
    }
    tls = 91;
    pthread_t a, b;
    CHECK(!pthread_create(&a, 0, worker, (void*)23), 1);
    CHECK(!pthread_create(&b, 0, worker, (void*)29), 2);
    void *ra, *rb;
    CHECK(!pthread_join(a, &ra) && !pthread_join(b, &rb), 3);
    CHECK(ra == (void*)23 && rb == (void*)29 && tls == 91 && total == 2000, 4);
    pthread_mutexattr_t attr;
    CHECK(!pthread_mutexattr_init(&attr) && !pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST), 5);
    CHECK(!pthread_mutex_init(&robust, &attr), 6);
    CHECK(!pthread_create(&a, 0, abandon, 0) && !pthread_join(a, &ra) && !ra, 7);
    CHECK(pthread_mutex_lock(&robust) == EOWNERDEAD, 8);
    CHECK(!pthread_mutex_consistent(&robust) && !pthread_mutex_unlock(&robust), 9);
    struct timespec past = {0, 1};
    CHECK(!pthread_mutex_lock(&mutex), 10);
    CHECK(pthread_cond_timedwait(&cond, &mutex, &past) == ETIMEDOUT, 11);
    CHECK(!pthread_mutex_unlock(&mutex), 12);
    CHECK(!pthread_create(&a, 0, spawn_child, 0) && !pthread_join(a, &ra) && !ra, 18);
    int retained = open("/dev/null", O_RDONLY);
    CHECK(retained >= 0 && !pthread_create(&a, 0, unshare_files, (void*)(intptr_t)retained) &&
          !pthread_join(a, &ra) && !ra && fcntl(retained, F_GETFD) == FD_CLOEXEC, 25);
    close(retained);
    struct sigaction action = {.sa_handler = handler};
    CHECK(!sigaction(SIGUSR2, &action, 0), 13);
    shared_fd = open("/dev/null", O_RDONLY);
    CHECK(shared_fd >= 0 && !pthread_create(&a, 0, spin, 0), 14);
    while (!atomic_load(&spinning)) {}
    CHECK(fcntl(shared_fd, F_GETFD) == -1 && errno == EBADF, 15);
    CHECK(!pthread_kill(a, SIGUSR2), 16);
    while (!atomic_load(&caught)) {}
    atomic_store(&stop, 1);
    CHECK(!pthread_join(a, &ra) && !ra && tls == 91, 17);
    int locked = open("/tmp/posix-lock", O_RDWR | O_CREAT, 0600);
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    CHECK(locked >= 0 && !fcntl(locked, F_SETLK, &lock), 19);
    pid_t child = fork();
    CHECK(child >= 0, 20);
    if (!child) {
        lock.l_type = F_WRLCK;
        if (fcntl(locked, F_GETLK, &lock) || lock.l_type != F_WRLCK || lock.l_pid != getppid()) _exit(1);
        lock.l_type = F_WRLCK;
        if (!fcntl(locked, F_SETLK, &lock) || errno != EAGAIN) _exit(2);
        close(locked); _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && !status, 21);
    close(locked);
    puts("pthread TLS, locks, conditions, signals, preemption and robust recovery ok");
    return 0;
}
