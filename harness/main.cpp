// Phase 1 harness: load a guest ELF out of the APK, run it under the sentry,
// report what happened.
//
// As with the probe, there is no Java and no classes.dex -- the manifest sets
// android:hasCode="false" and the framework's NativeActivity hosts us, so this
// measures the same C++ environment the real sentry will live in.

#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_activity.h>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <utility>
#include <string>
#include <vector>

#include "elf_loader.h"
#include "exec_memory.h"
#include "guest_layout.h"
#include "sentry.h"
#include "stub.h"

namespace {

constexpr const char* kTag = "goblin-sentry";

struct Args {
    std::string internal_data;
    AAssetManager* assets = nullptr;
};

std::vector<std::string>* g_report = nullptr;

void Say(const std::string& line) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "%s", line.c_str());
    if (g_report) g_report->push_back(line);
}

__attribute__((format(printf, 1, 2))) void Sayf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Say(buf);
}

bool ReadAsset(AAssetManager* mgr, const char* name, std::vector<uint8_t>* out) {
    if (mgr == nullptr) return false;
    AAsset* a = AAssetManager_open(mgr, name, AASSET_MODE_BUFFER);
    if (a == nullptr) return false;
    const off64_t len = AAsset_getLength64(a);
    const void* buf = AAsset_getBuffer(a);
    bool ok = buf != nullptr && len > 0;
    if (ok) {
        out->assign(static_cast<const uint8_t*>(buf),
                    static_cast<const uint8_t*>(buf) + len);
    }
    AAsset_close(a);
    return ok;
}

// The guest runs on a thread of its own because the seccomp filter it installs
// is permanent -- the thread can never go back to being an ordinary one.
void* GuestThread(void* arg) {
    auto* run = static_cast<std::pair<goblin::LoadedImage*, goblin::Sentry*>*>(arg);
    static goblin::RunResult result;
    result = goblin::RunGuest(*run->first, run->second);
    return &result;
}

struct GuestSpec {
    const char* asset;
    const char* expect_stdout;
    bool show_trace;
};

bool RunGuestAsset(AAssetManager* assets, const GuestSpec& spec) {
    Say("");
    Sayf("-- guest: %s --", spec.asset);

    std::vector<uint8_t> elf;
    if (!ReadAsset(assets, spec.asset, &elf)) {
        Sayf("FAILED: could not read guest asset '%s'", spec.asset);
        return false;
    }
    Sayf("image                %zu bytes", elf.size());

    std::string err;
    goblin::LoadedImage img;
    if (!goblin::LoadElf(elf.data(), elf.size(), &img, &err)) {
        Sayf("FAILED to load: %s", err.c_str());
        return false;
    }
    Sayf("loaded               bias=%#lx entry=%#lx", img.bias, img.entry);
    Sayf("  mapped span        %#lx - %#lx", img.image_start, img.image_end);
    Sayf("  executable         %#lx - %#lx", img.exec_start, img.exec_end);
    Sayf("  backing            %s",
         img.storage.sealed_memfd() ? "sealed memfd" : "anonymous");

    goblin::Sentry sentry([](const std::string& s) { Say("  " + s); });

    auto run = std::make_pair(&img, &sentry);
    pthread_t th;
    if (pthread_create(&th, nullptr, GuestThread, &run) != 0) {
        Say("FAILED: could not create guest thread");
        return false;
    }
    void* ret = nullptr;
    pthread_join(th, &ret);
    const auto& r = *static_cast<goblin::RunResult*>(ret);

    if (spec.show_trace) {
        Say("  syscall trace:");
        for (const std::string& line : sentry.trace()) Say("    " + line);
        if (sentry.trace_truncated()) {
            Sayf("    ... %llu more",
                 static_cast<unsigned long long>(sentry.syscall_count() -
                                                 sentry.trace().size()));
        }
    }

    if (!r.error.empty()) {
        Sayf("FAILED: %s", r.error.c_str());
        return false;
    }
    if (!r.exited) {
        Say("FAILED: guest stopped without exiting");
        return false;
    }

    Sayf("exited               status %d after %llu syscalls", r.status,
         static_cast<unsigned long long>(r.syscalls));
    if (r.ns_per_syscall > 0 && r.syscalls >= 1000) {
        // Only meaningful once entry cost is amortised over many syscalls.
        Sayf("cost                 %.0f ns per syscall total", r.ns_per_syscall);
        Sayf("  of which             %.0f ns in the sentry, %.0f ns in signal delivery",
             r.ns_in_handler, r.ns_per_syscall - r.ns_in_handler);
    }

    const bool ok = r.status == 0 && sentry.guest_stdout() == spec.expect_stdout;
    if (!ok) {
        Sayf("FAILED: status %d, stdout %s", r.status,
             sentry.guest_stdout().c_str());
    }
    return ok;
}

void RunPhase1(const Args& args) {
    Say("== goblin-linux phase 1: loader + sentry ==");
    Sayf("page size            %ld", sysconf(_SC_PAGESIZE));
    Sayf("memfd exec           %s", goblin::MemfdExecSupported()
                                        ? "supported (sealed, shared)"
                                        : "unsupported (anonymous fallback)");

    std::string err;
    if (!goblin::GuestWindow::Reserve(&err)) {
        Sayf("FAILED: %s", err.c_str());
        return;
    }
    Sayf("guest window         %#lx - %#lx (4 GiB aligned)",
         static_cast<unsigned long>(goblin::GuestWindow::start()),
         static_cast<unsigned long>(goblin::GuestWindow::end()));

    const GuestSpec guests[] = {
        {"hello", "hello, world\n", true},
        {"bench", "bench done\n", false},
    };

    bool all = true;
    for (const GuestSpec& g : guests) all = RunGuestAsset(args.assets, g) && all;

    Say("");
    Say(all ? "PHASE 1 PASS" : "PHASE 1 FAIL");
}

void* Main(void* arg) {
    std::unique_ptr<Args> args(static_cast<Args*>(arg));
    std::vector<std::string> report;
    g_report = &report;

    RunPhase1(*args);

    std::string text;
    for (const std::string& l : report) {
        text += l;
        text += '\n';
    }
    const std::string out = args->internal_data + "/phase1-report.txt";
    int fd = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        (void)!write(fd, text.data(), text.size());
        close(fd);
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "PHASE1 COMPLETE");
    return nullptr;
}

void OnDestroy(ANativeActivity*) {}

}  // namespace

extern "C" __attribute__((visibility("default")))
void ANativeActivity_onCreate(ANativeActivity* activity, void*, size_t) {
    activity->callbacks->onDestroy = OnDestroy;

    auto* args = new Args;
    args->internal_data =
        activity->internalDataPath ? activity->internalDataPath : "/data/local/tmp";
    args->assets = activity->assetManager;

    pthread_t th;
    if (pthread_create(&th, nullptr, Main, args) == 0) {
        pthread_detach(th);
    } else {
        delete args;
        __android_log_print(ANDROID_LOG_ERROR, kTag, "could not start main thread");
    }
}
