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
#include <signal.h>
#include <unistd.h>

#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <utility>
#include <string>
#include <vector>
#include <filesystem>

#include "elf_loader.h"
#include "exec_memory.h"
#include "guest_layout.h"
#include "sentry.h"
#include "stub.h"
#include "core_tests.h"
#include "debian_tests.h"
#include "vfs.h"
#include "vfs_tests.h"
#include "persistent_tests.h"
#include "developer_tests.h"
#include "recovery_tests.h"
#include "harness_service.h"

namespace {

constexpr const char* kTag = "goblin-sentry";

struct Args {
    std::string internal_data;
    AAssetManager* assets = nullptr;
    int mode = 0;
};

std::vector<std::string>* g_report = nullptr;
std::function<void(const std::string&)> g_output;

void Say(const std::string& line) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "%s", line.c_str());
    if (g_report) g_report->push_back(line);
    if (g_output) g_output(line + "\r\n");
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

struct GuestRun {
    goblin::LoadedImage* image;
    goblin::Sentry* sentry;
    bool expose_test_gate;
};

// Each run uses a fresh launcher thread; the filter exists only in its child.
void* GuestThread(void* arg) {
    auto* run = static_cast<GuestRun*>(arg);
    static goblin::RunResult result;
    result = goblin::RunGuest(*run->image, run->sentry, run->expose_test_gate);
    return &result;
}

struct GuestSpec {
    const char* asset;
    const char* expect_stdout;
    bool show_trace;
    bool expose_test_gate = false;
    int expected_signal = 0;
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
    if (!goblin::GuestWindow::Reset()) { Say("FAILED: resetting guest windows"); return false; }
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

    GuestRun run{&img, &sentry, spec.expose_test_gate};
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

    if (spec.expected_signal && r.fault_signal == spec.expected_signal) {
        Sayf("expected fault       %s", r.error.c_str());
        return true;
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

    const bool ok = !spec.expected_signal && r.status == 0 &&
                    sentry.guest_stdout() == spec.expect_stdout;
    if (!ok) {
        Sayf("FAILED: status %d, stdout %s", r.status,
             sentry.guest_stdout().c_str());
    }
    return ok;
}

void RunPhase1(const Args& args) {
    if (args.mode) {
        Sayf("page size            %ld", sysconf(_SC_PAGESIZE));
        bool ok = args.mode == 1 ? RunPersistentTests(args.internal_data, args.assets, Say) :
                  args.mode == 2 ? RunDeveloperTests(args.internal_data, args.assets, Say) :
                  args.mode == 3 ? RunRecoveryTests(args.internal_data, args.assets, Say) :
                  args.mode == 4 ? RunDeveloperTests(args.internal_data, args.assets, Say, true) : false;
        Say(ok ? "GOBLIN PASS" : "GOBLIN FAIL");
        return;
    }
    Say("== GoblinReactor: isolated stub and memory ABI regression harness ==");
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
        {"fixed", "fixed ELF low memory and fork ok\n", true},
        {"bench", "bench done\n", false},
        {"memtest", "memtest ok\n", true},
        {"memerrors", "memory errors ok\n", true},
        {"time_query", "time query ok\n", true},
        {"isolation", "isolation and TLS ok\n", true, true},
        {"oldcode", "", false, true, SIGSEGV},
        {"badstack", "", true},
        {"process", "fork pipes shared memory and futex ok\n", true},
        {"signals", "signals masks altstack and EINTR ok\n", true},
    };

    bool all = RunCoreTests(Say);
    for (const GuestSpec& g : guests) all = RunGuestAsset(args.assets, g) && all;

    std::vector<uint8_t> rootfs;
    const std::string root = args.internal_data + "/rootfs-" + std::to_string(getpid());
    if (!ReadAsset(args.assets, "rootfs.pack", &rootfs) ||
        !goblin::InstallRootfs(rootfs.data(), rootfs.size(), root, &err)) {
        Say("FAILED: Debian rootfs: " + err); all = false;
    } else {
        rootfs.clear(); rootfs.shrink_to_fit();
        all = RunVfsTests(root, Say) && all;
        std::vector<uint8_t> test_elf;
        goblin::Vfs vfs;
        for (const char* fixture : {"fdexec", "filemap", "pthreads", "network", "proc_ipc", "identity", "setid"}) {
            if (!ReadAsset(args.assets, fixture, &test_elf) || !vfs.Mount(root, &err)) {
                Say("FAILED: reading exec regression fixture"); all = false;
            } else {
                int fd = vfs.Open(std::string("/tmp/") + fixture, O_WRONLY | O_CREAT | O_EXCL, 0755);
                if (fd < 0 || write(fd, test_elf.data(), test_elf.size()) != static_cast<ssize_t>(test_elf.size())) {
                    Say("FAILED: installing exec regression fixture"); all = false;
                }
                if (fd >= 0) close(fd);
            }
        }
        all = RunDebianTests(root, Say) && all;
        std::error_code cleanup;
        std::filesystem::remove_all(root, cleanup);
        if (cleanup) { Say("FAILED: rootfs cleanup: " + cleanup.message()); all = false; }
    }

    Say("");
    Say(all ? "GOBLIN PASS" : "GOBLIN FAIL");
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
    const std::string temporary = out + ".new";
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        size_t written = 0;
        while (written < text.size()) {
            ssize_t n = write(fd, text.data() + written, text.size() - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            written += n;
        }
        bool complete = written == text.size() && fsync(fd) == 0;
        close(fd);
        if (complete) rename(temporary.c_str(), out.c_str());
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "GOBLIN COMPLETE");
    g_report = nullptr;
    return nullptr;
}

void OnDestroy(ANativeActivity*) {}

}  // namespace

void RunHarnessForService(const std::string& data, AAssetManager* assets, int mode,
                          const std::function<void(const std::string&)>& output) {
    g_output = output;
    auto* args = new Args; args->internal_data = data; args->assets = assets; args->mode = mode;
    Main(args);
    g_output = {};
}

extern "C" __attribute__((visibility("default")))
void ANativeActivity_onCreate(ANativeActivity* activity, void*, size_t) {
    activity->callbacks->onDestroy = OnDestroy;
    ANativeActivity_setWindowFlags(activity, 0x80 /* FLAG_KEEP_SCREEN_ON */, 0);

    auto* args = new Args;
    args->internal_data =
        activity->internalDataPath ? activity->internalDataPath : "/data/local/tmp";
    args->assets = activity->assetManager;
    args->mode = access((args->internal_data + "/run-persistent").c_str(), F_OK) == 0 ? 1 : 0;

    pthread_t th;
    if (pthread_create(&th, nullptr, Main, args) == 0) {
        pthread_detach(th);
    } else {
        delete args;
        __android_log_print(ANDROID_LOG_ERROR, kTag, "could not start main thread");
    }
}
