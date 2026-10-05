#include "session.h"
#include "harness_service.h"
#include "deployment.h"
#include "../terminal/engine.h"
#include <android/asset_manager_jni.h>
#include <jni.h>
#include <sys/file.h>
#include <thread>
#include <memory>
#include <sstream>

namespace {
std::mutex state_mutex;
std::shared_ptr<goblin::SessionControl> session;
std::shared_ptr<goblin::RuntimeControl> runtime;
std::atomic<int> selected{0};
std::atomic<bool> running{false};
std::string state = "Stopped";
void State(const std::string& text) { std::lock_guard<std::mutex> lock(state_mutex); state = text; }
std::shared_ptr<goblin::RuntimeControl> Runtime() { std::lock_guard<std::mutex> lock(state_mutex); return runtime; }
std::shared_ptr<goblin::TerminalSession> Terminal(int id) {
    auto r = Runtime(); if (r) for (auto& s : r->Sessions()) if (s->id == id) return s; return {};
}
std::shared_ptr<goblin::SessionControl> Current() {
    if (auto s = Terminal(selected)) return s->control;
    std::lock_guard<std::mutex> lock(state_mutex); return session;
}
void Worker(const std::string& data, JavaVM* vm, jobject asset_reference) {
    JNIEnv* env = nullptr;
    vm->AttachCurrentThread(&env, nullptr);
    AAssetManager* assets = AAssetManager_fromJava(env, asset_reference);
    std::shared_ptr<goblin::SessionControl> control;
    { std::lock_guard<std::mutex> guard(state_mutex); control = session; }
    auto linux_runtime = Runtime();
    std::string error;
    goblin::HostFile lock(open((data + "/debian.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
    bool ok = lock.fd >= 0 && flock(lock.fd, LOCK_EX | LOCK_NB) == 0;
    if (!ok) error = "Another Debian session is active.";
    const std::string root = data + "/debian";
    if (ok) {
        State("Preparing Debian");
        AAsset* seed = AAssetManager_open(assets, "debian.pack", AASSET_MODE_BUFFER);
        if (!seed) { ok = false; error = "Debian seed is missing from the application."; }
        else {
            ok = goblin::EnsurePersistentRoot(static_cast<const uint8_t*>(AAsset_getBuffer(seed)), AAsset_getLength64(seed), root, &error);
            AAsset_close(seed);
        }
    }
    goblin::RunLimits limits; limits.wall_time_ms = 1200000;
    if (ok && !control->stop) {
        State("Installing Debian packages");
        control->Append("Preparing your Debian environment…\r\n");
        auto result = goblin::RunInRoot(root, {"/bin/bash", "/usr/local/sbin/goblin-bootstrap"}, limits, control.get(), false, nullptr, nullptr);
        ok = result.exited && !result.status && result.error.empty();
        if (!ok) error = result.error.empty() ? "Debian setup failed. Its package state is retained; restarting retries setup." : result.error;
    }
    if (ok && !control->stop) {
        State("Preparing accounts");
        if (!goblin::EnsureDefaultUser(root,limits,control.get(),&error)) {
            control->Append("\r\n" + error + "\r\nUse Root terminal in the menu to repair the account.\r\n");
            error.clear();
        }
    }
    if (ok && !control->stop) {
        State("Configuring Debian");
        if (!DeployGuestFeatures(root,assets,limits,control.get(),&error)) ok=false;
    }
    env->DeleteGlobalRef(asset_reference);
    vm->DetachCurrentThread();
    if (ok && !control->stop) {
        State("Running"); limits.wall_time_ms = 0; limits.output_bytes = 64ull << 20;
        control->Append("\r\n");
        auto result = goblin::RunInRoot(root,{"/sbin/init"},limits,control.get(),false,nullptr,nullptr,linux_runtime.get());
        if (!result.error.empty() && !control->stop && !linux_runtime->stop) error = result.error;
    }
    if (!error.empty()) control->Append("\r\n" + error + "\r\n");
    for (const auto& terminal : linux_runtime->Sessions()) if (terminal->state < goblin::TerminalSession::kExited) {
        if (!error.empty()) terminal->control->Append("\r\n" + error + "\r\n");
        terminal->state = error.empty() ? goblin::TerminalSession::kExited : goblin::TerminalSession::kFailed;
    }
    State(error.empty() || control->stop ? "Stopped" : "Setup or session failed");
    running = false;
}
}

extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_SessionService_startNative(JNIEnv* env, jclass, jstring directory, jobject assets) {
    if (running.exchange(true)) return;
    if (auto previous = Runtime()) for (const auto& s : previous->Sessions()) GoblinKittyClose(s->id);
    const char* path = env->GetStringUTFChars(directory, nullptr);
    std::string data(path); env->ReleaseStringUTFChars(directory, path);
    auto r = std::make_shared<goblin::RuntimeControl>();
    r->prepare = [](goblin::TerminalSession& terminal) {
        int id = terminal.id;
        terminal.control->output_handler=[id](const std::string& text) { GoblinKittyFeedSession(id,text.data(),text.size()); };
        terminal.control->terminal_input=[id] { char bytes[65536];size_t n=GoblinKittyTakeSessionInput(id,bytes,sizeof(bytes));return std::string(bytes,n); };
    };
    r->retire = [](int id) { GoblinKittyClose(id); };
    auto first = r->Open("goblin");
    { std::lock_guard<std::mutex> lock(state_mutex); runtime = r; session = std::make_shared<goblin::SessionControl>(); session->output_handler = first->control->output_handler; state = "Starting"; selected = first->id; }
    GoblinKittySelect(first->id); GoblinKittyReset();
    JavaVM* vm; env->GetJavaVM(&vm);
    jobject reference = env->NewGlobalRef(assets);
    std::thread(Worker, data, vm, reference).detach();
}
extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_SessionService_stopNative(JNIEnv*, jclass) {
    if (auto r = Runtime()) r->stop = true;
    std::lock_guard<std::mutex> lock(state_mutex); if (session) session->stop = true;
}
extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_SessionService_testsNative(JNIEnv* env, jclass, jstring directory, jobject assets, jint mode) {
    if (running.exchange(true)) return;
    const char* path = env->GetStringUTFChars(directory, nullptr);
    std::string data(path); env->ReleaseStringUTFChars(directory, path);
    { std::lock_guard<std::mutex> lock(state_mutex); runtime.reset(); selected = 0; session = std::make_shared<goblin::SessionControl>(); state = "Running acceptance tests"; }
    JavaVM* vm; env->GetJavaVM(&vm); jobject reference = env->NewGlobalRef(assets);
    std::thread([data, vm, reference, mode] {
        JNIEnv* worker; vm->AttachCurrentThread(&worker, nullptr);
        AAssetManager* manager = AAssetManager_fromJava(worker, reference);
        auto control = Current();
        RunHarnessForService(data, manager, mode, [control](const std::string& text) { control->Append(text); });
        worker->DeleteGlobalRef(reference); vm->DetachCurrentThread();
        State("Tests complete"); running = false;
    }).detach();
}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_goblinreactor_sentry_SessionService_runningNative(JNIEnv*, jclass) { return running; }
extern "C" JNIEXPORT jbyteArray JNICALL Java_dev_goblinreactor_sentry_SessionService_transcriptNative(JNIEnv* env, jclass) {
    auto s = Current(); std::string bytes = s ? s->Transcript() : "";
    jbyteArray result = env->NewByteArray(bytes.size());
    if (result) env->SetByteArrayRegion(result, 0, bytes.size(), reinterpret_cast<const jbyte*>(bytes.data()));
    return result;
}
extern "C" JNIEXPORT jstring JNICALL Java_dev_goblinreactor_sentry_SessionService_stateNative(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(state_mutex); return env->NewStringUTF(state.c_str());
}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_goblinreactor_sentry_SessionService_inputNative(JNIEnv* env, jclass, jbyteArray value) {
    auto s = Current(); if (!s || !running || s->stop) return false;
    if (auto terminal = Terminal(selected); terminal && terminal->state >= goblin::TerminalSession::kExited) return false;
    std::string bytes(env->GetArrayLength(value), '\0');
    env->GetByteArrayRegion(value, 0, bytes.size(), reinterpret_cast<jbyte*>(bytes.data()));
    return s->Input(bytes);
}
extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_SessionService_resizeNative(JNIEnv*, jclass, jint rows, jint columns) {
    if (auto s = Current()) s->Resize(rows > 0 ? rows : 1, columns > 0 ? columns : 1);
}

extern "C" JNIEXPORT jint JNICALL Java_dev_goblinreactor_sentry_SessionService_selectedNative(JNIEnv*, jclass) { return selected; }
extern "C" JNIEXPORT jint JNICALL Java_dev_goblinreactor_sentry_SessionService_terminalStateNative(JNIEnv*, jclass, jint id) {
    auto s = Terminal(id); return s ? s->state.load() : goblin::TerminalSession::kExited;
}
extern "C" JNIEXPORT jstring JNICALL Java_dev_goblinreactor_sentry_SessionService_terminalsNative(JNIEnv* env, jclass) {
    std::ostringstream list;
    if (auto r = Runtime()) for (const auto& s : r->Sessions()) if (s->state < goblin::TerminalSession::kExited)
        list << s->id << '\t' << s->user << '\t' << s->pid << '\n';
    return env->NewStringUTF(list.str().c_str());
}
extern "C" JNIEXPORT jint JNICALL Java_dev_goblinreactor_sentry_SessionService_openTerminalNative(JNIEnv* env, jclass, jstring name, jboolean create) {
    auto r = Runtime(); if (!r || !running || r->stop) return 0;
    const char* text = env->GetStringUTFChars(name,nullptr); std::string user(text); env->ReleaseStringUTFChars(name,text);
    auto s = r->Open(user,create); if (!s) return 0;
    selected = s->id; GoblinKittySelect(s->id); return s->id;
}
extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_SessionService_selectNative(JNIEnv*, jclass, jint id) {
    auto s = Terminal(id); if (s && s->state < goblin::TerminalSession::kExited) { selected = id; GoblinKittySelect(id); }
}
extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_SessionService_closeTerminalNative(JNIEnv*, jclass, jint id) {
    if (auto s = Terminal(id)) s->control->stop = true;
}
extern "C" JNIEXPORT void JNICALL Java_dev_goblinreactor_sentry_SessionService_releaseTerminalNative(JNIEnv*, jclass, jint id) {
    auto s = Terminal(id); if (!s || s->state >= goblin::TerminalSession::kExited) { GoblinKittyClose(id); if (selected == id) selected = 0; }
}
