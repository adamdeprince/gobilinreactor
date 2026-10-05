#include "disk-backup.h"
#include <atomic>
#include <jni.h>

namespace { std::atomic<uint64_t> copied{0}; }
extern "C" JNIEXPORT jstring JNICALL Java_dev_goblinlinux_sentry_MaintenanceService_previousNative(JNIEnv* env, jclass, jstring path, jstring file) {
    const char* p = env->GetStringUTFChars(path, nullptr); std::string directory(p); env->ReleaseStringUTFChars(path, p);
    const char* f = env->GetStringUTFChars(file, nullptr); std::string name(f); env->ReleaseStringUTFChars(file, f);
    copied = 0;
    try { return env->NewStringUTF(goblin_disk::restorePrevious(directory, name, [](uint64_t n) { copied = n; }).c_str()); }
    catch (const std::exception& error) { env->ThrowNew(env->FindClass("java/io/IOException"), error.what()); return nullptr; }
}
extern "C" JNIEXPORT jlong JNICALL Java_dev_goblinlinux_sentry_MaintenanceService_progressNative(JNIEnv*, jclass) { return copied; }
extern "C" JNIEXPORT jstring JNICALL Java_dev_goblinlinux_sentry_MaintenanceService_transferNative(JNIEnv* env, jclass, jstring path, jint fd, jboolean restore) {
    const char* text = env->GetStringUTFChars(path, nullptr); std::string directory(text); env->ReleaseStringUTFChars(path, text);
    copied = 0;
    try {
        auto progress = [](uint64_t n) { copied = n; };
        if (!restore) { goblin_disk::exportDisk(directory, fd, progress); return env->NewStringUTF(""); }
        auto previous = goblin_disk::importDisk(directory, fd, progress); return env->NewStringUTF(previous.c_str());
    } catch (const std::exception& error) {
        env->ThrowNew(env->FindClass("java/io/IOException"), error.what()); return nullptr;
    }
}
