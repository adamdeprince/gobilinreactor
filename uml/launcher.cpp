// The app-zygote service owns the UML process tree. No private path access is
// needed: Android Binder grants the disk, boot assets, sockets and disk lock.
#include <jni.h>
#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <string>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "fd-cleanup.h"

namespace {
std::mutex mutex;
std::condition_variable stopped;
pid_t machine = -1;
std::string String(JNIEnv* env, jstring value) {
    const char* bytes = env->GetStringUTFChars(value, nullptr);
    std::string result(bytes); env->ReleaseStringUTFChars(value, bytes); return result;
}
void Error(JNIEnv* env, const char* message) {
    env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), message);
}
}

extern "C" JNIEXPORT jint JNICALL Java_dev_goblinreactor_sentry_KernelService_spawn(
        JNIEnv* env, jclass, jstring executable, jobjectArray arguments,
        jintArray descriptors, jintArray destinations, jint status_fd) {
    std::unique_lock<std::mutex> guard(mutex);
    if (machine > 0) { Error(env, "A environment machine is already active"); return -1; }
    std::string path = String(env, executable);
    std::vector<std::string> strings;
    for (int i = 0; i < env->GetArrayLength(arguments); ++i) {
        auto item = static_cast<jstring>(env->GetObjectArrayElement(arguments, i));
        strings.push_back(String(env, item)); env->DeleteLocalRef(item);
    }
    std::vector<char*> argv; for (auto& item : strings) argv.push_back(item.data()); argv.push_back(nullptr);
    char env_path[] = "PATH=/system/bin", env_home[] = "HOME=/", env_tmp[] = "TMPDIR=/";
    char* environment[] = {env_path, env_home, env_tmp, nullptr};
    int count = env->GetArrayLength(descriptors), last = 2;
    std::vector<jint> sources(count), targets(count);
    env->GetIntArrayRegion(descriptors, 0, count, sources.data());
    env->GetIntArrayRegion(destinations, 0, count, targets.data());
    std::vector<int> copies;
    int disk_lock = -1, status = fcntl(status_fd, F_DUPFD_CLOEXEC, 64);
    for (int i = 0; i < count; ++i) {
        last = std::max(last, targets[i]);
        copies.push_back(fcntl(sources[i], F_DUPFD_CLOEXEC, 64));
        if (targets[i] == 9) disk_lock = fcntl(sources[i], F_DUPFD_CLOEXEC, 64);
    }
    auto close_copies = [&] { for (int fd : copies) if (fd >= 0) close(fd); };
    if (status < 0 || disk_lock < 0 || std::find(copies.begin(), copies.end(), -1) != copies.end() ||
        prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        close_copies(); if (status >= 0) close(status); if (disk_lock >= 0) close(disk_lock);
        Error(env, "Cannot retain environment descriptors or supervise workers"); return -1;
    }
    const pid_t parent = getpid();
    pid_t child = fork();
    if (!child) {
        if (setsid() < 0 || prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != parent) _exit(125);
        for (int i = 0; i < count; ++i) if (dup2(copies[i], targets[i]) < 0) _exit(126);
        if (goblin_cleanup_fds(last + 1, 0) < 0) _exit(126);
        rlimit files{};
        if (getrlimit(RLIMIT_NOFILE, &files) == 0) { files.rlim_cur = files.rlim_max; setrlimit(RLIMIT_NOFILE, &files); }
        execve(path.c_str(), argv.data(), environment); _exit(127);
    }
    close_copies();
    if (child < 0) {
        close(status); close(disk_lock); Error(env, "Cannot fork guest kernel"); return -1;
    }
    machine = child;
    std::thread([child, status, disk_lock] {
        sigset_t blocked; sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
        int result = 127 << 8;
        while (waitpid(child, &result, 0) < 0 && errno == EINTR) {}
        // Kernel exit must release every host worker before releasing the disk
        // lock. Subreaping catches workers orphaned by a crash during boot/run.
        {
            std::lock_guard<std::mutex> guard(mutex);
            kill(-child, SIGKILL);
        }
        while (waitpid(-child, nullptr, 0) >= 0 || errno == EINTR) {}
        close(disk_lock);
        ssize_t written; do { written = write(status, &result, sizeof(result)); } while (written < 0 && errno == EINTR);
        close(status);
        { std::lock_guard<std::mutex> guard(mutex); machine = -1; }
        stopped.notify_all();
    }).detach();
    return child;
}

extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_KernelService_stop(JNIEnv*, jclass) {
    std::unique_lock<std::mutex> guard(mutex);
    if (machine <= 0) return;
    kill(-machine, SIGKILL);
    // Also cover the short interval before the child creates its process group.
    kill(machine, SIGKILL);
    stopped.wait(guard, [] { return machine <= 0; });
}
