// NativeActivity entry point for the goblin-linux capability probe.
//
// There is no Java in this APK at all -- the manifest declares
// android:hasCode="false" and points android.app.lib_name at this library, so the
// framework's own NativeActivity class hosts us and no classes.dex is produced.
// That keeps the probe honest: it is exactly the C++ environment the real sentry
// will run in.

#include <android/log.h>
#include <android/native_activity.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <memory>
#include <string>

#include "probe.h"

namespace {

constexpr const char* kTag = "goblin-probe";

// The APK's extracted native library directory, discovered without JNI: our own
// library is already loaded, so dladdr can name the file it came from.
std::string NativeLibDir() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&NativeLibDir), &info) == 0 || !info.dli_fname) {
        return {};
    }
    std::string path(info.dli_fname);
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

void LogReport(const std::string& report) {
    // logcat truncates long messages, so emit one line at a time.
    size_t start = 0;
    while (start < report.size()) {
        size_t end = report.find('\n', start);
        if (end == std::string::npos) end = report.size();
        __android_log_print(ANDROID_LOG_INFO, kTag, "%.*s",
                            static_cast<int>(end - start), report.data() + start);
        start = end + 1;
    }
}

struct ThreadArgs {
    std::string internal_data;
};

void* RunProbes(void* arg) {
    std::unique_ptr<ThreadArgs> args(static_cast<ThreadArgs*>(arg));

    goblin::ProbePaths paths;
    paths.internal_data = args->internal_data;
    paths.native_lib_dir = NativeLibDir();

    __android_log_print(ANDROID_LOG_INFO, kTag, "probing (data=%s, libs=%s)",
                        paths.internal_data.c_str(), paths.native_lib_dir.c_str());

    const std::string report = goblin::RunAllProbes(paths);
    LogReport(report);

    const std::string out = paths.internal_data + "/probe-report.txt";
    int fd = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        ssize_t written = write(fd, report.data(), report.size());
        close(fd);
        __android_log_print(ANDROID_LOG_INFO, kTag, "report written to %s (%zd bytes)",
                            out.c_str(), written);
    } else {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "could not write %s", out.c_str());
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "PROBE COMPLETE");
    return nullptr;
}

void OnDestroy(ANativeActivity*) {}

}  // namespace

extern "C" __attribute__((visibility("default")))
void ANativeActivity_onCreate(ANativeActivity* activity, void* /*saved_state*/,
                              size_t /*saved_state_size*/) {
    activity->callbacks->onDestroy = OnDestroy;

    auto* args = new ThreadArgs;
    args->internal_data = activity->internalDataPath ? activity->internalDataPath : "/data/local/tmp";

    // Probing forks, installs seccomp filters and deliberately provokes faults;
    // none of that belongs on the UI thread.
    pthread_t th;
    if (pthread_create(&th, nullptr, RunProbes, args) == 0) {
        pthread_detach(th);
    } else {
        delete args;
        __android_log_print(ANDROID_LOG_ERROR, kTag, "could not start probe thread");
    }
}
