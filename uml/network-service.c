// passt's event loop runs in its own Android-managed service process. It must
// keep ART/Binder's descriptors intact, and uses the granted packet FD as-is.
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int goblin_passt_main(int argc, char **argv);

void goblin_network_namespace_unavailable(void)
{
    fputs("Managed networking cannot enter a desktop host network namespace\n", stderr);
    _exit(125);
}

JNIEXPORT void JNICALL Java_dev_goblinlinux_sentry_NetworkService_run(
        JNIEnv *env, jclass type, jobjectArray arguments, jint packets, jint log, jint status)
{
    (void)type;
    int argc = (*env)->GetArrayLength(env, arguments);
    char **argv = calloc(argc + 1, sizeof(*argv));
    if (!argv) _exit(125);
    for (int i = 0; i < argc; ++i) {
        jstring value = (jstring)(*env)->GetObjectArrayElement(env, arguments, i);
        const char *text = (*env)->GetStringUTFChars(env, value, NULL);
        argv[i] = strdup(text);
        (*env)->ReleaseStringUTFChars(env, value, text);
        (*env)->DeleteLocalRef(env, value);
        if (!argv[i]) _exit(125);
    }
    char descriptor[32]; snprintf(descriptor, sizeof(descriptor), "%d", packets);
    for (int i = 1; i + 1 < argc; ++i) if (!strcmp(argv[i], "--fd")) {
        free(argv[i + 1]); argv[i + 1] = strdup(descriptor);
        if (!argv[i + 1]) _exit(125);
        break;
    }
    if (dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) _exit(126);
    int result = goblin_passt_main(argc, argv) << 8;
    ssize_t ignored = write(status, &result, sizeof(result)); (void)ignored;
    _exit(result >> 8);
}
