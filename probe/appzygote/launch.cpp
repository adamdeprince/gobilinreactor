#include <jni.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

extern "C" JNIEXPORT jint JNICALL
Java_dev_goblinlinux_zygoteprobe_WorkerService_launch(JNIEnv* env, jclass,
        jstring executable, jstring dataPath, jint report, jint data, jint network,
        jint children, jint seconds) {
    const char* path = env->GetStringUTFChars(executable, nullptr);
    const char* file = env->GetStringUTFChars(dataPath, nullptr);
    char count[24], duration[24];
    snprintf(count, sizeof(count), "%d", children); snprintf(duration, sizeof(duration), "%d", seconds);
    int fds[] = {fcntl(report, F_DUPFD_CLOEXEC, 64), fcntl(data, F_DUPFD_CLOEXEC, 64), fcntl(network, F_DUPFD_CLOEXEC, 64)};
    pid_t parent = getpid(), pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(124);
        if (dup2(fds[0], 1) < 0 || dup2(fds[0], 2) < 0 || dup2(fds[1], 3) < 0 || dup2(fds[2], 4) < 0) _exit(125);
        for (int fd : fds) close(fd);
        char* argv[] = {const_cast<char*>(path), count, duration, const_cast<char*>(file), nullptr};
        char* environment[] = {const_cast<char*>("PATH=/system/bin"), nullptr};
        execve(path, argv, environment);
        dprintf(2, "FAIL exec packaged helper errno=%d\n", errno); _exit(126);
    }
    for (int fd : fds) if (fd >= 0) close(fd);
    env->ReleaseStringUTFChars(executable, path); env->ReleaseStringUTFChars(dataPath, file);
    if (pid < 0) return -errno;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -errno;
    return status;
}
