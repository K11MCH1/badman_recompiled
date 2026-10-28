// JNI side of the Android app (org.psprecomp.badman.GameActivity). It runs the
// same native main as the desktop build on a game thread and connects the
// activity's surface, input and dialogs to the game through AndroidBridge.
#include "badman_android_bridge.hpp"
#include "badman_profiler.hpp"
#include <android/native_window_jni.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

int badman_native_main(int argc, char **argv);

namespace {
std::atomic<int> exit_code{-1};
std::atomic<bool> started{};
std::vector<std::string> arguments;

std::string text(JNIEnv *env, jstring value) {
    if (!value) return {};
    const auto *chars = env->GetStringUTFChars(value, nullptr);
    std::string result(chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return result;
}
std::vector<std::string> texts(JNIEnv *env, jobjectArray values) {
    std::vector<std::string> result;
    const auto count = values ? env->GetArrayLength(values) : 0;
    for (jsize i = 0; i < count; ++i) {
        const auto value = static_cast<jstring>(env->GetObjectArrayElement(values, i));
        result.push_back(text(env, value));
        env->DeleteLocalRef(value);
    }
    return result;
}
jstring java_string(JNIEnv *env, const std::u16string &value) {
    return env->NewString(reinterpret_cast<const jchar *>(value.data()), static_cast<jsize>(value.size()));
}
void *run_game(void *) {
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    // Development profiling of the game thread (never set by a normal Play).
    if (const auto profile = std::getenv("BADMAN_PROFILE"); profile && *profile)
        badman::SamplingProfiler::start_for_current_thread(profile);
    const int code = badman_native_main(static_cast<int>(arguments.size()), argv.data());
    badman::SamplingProfiler::finish();
    std::fflush(stdout);
    std::fflush(stderr);
    exit_code = code;
    return nullptr;
}
}

extern "C" {
JNIEXPORT jboolean JNICALL Java_org_psprecomp_badman_GameActivity_nativeStart(JNIEnv *env, jclass,
    jobjectArray argv, jobjectArray environment, jstring log) {
    if (started.exchange(true)) return JNI_FALSE;
    // Output goes to the session log, which the app shows if the game stops with an error.
    const auto path = text(env, log);
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); setvbuf(stdout, nullptr, _IOLBF, 0); }
    const auto variables = texts(env, environment);
    for (std::size_t i = 0; i + 1 < variables.size(); i += 2) setenv(variables[i].c_str(), variables[i + 1].c_str(), 1);
    arguments = texts(env, argv);
    badman::AndroidBridge::instance().attach();
    // The desktop executable runs with an 8 MiB main stack; give the game thread room beyond that.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 16u << 20);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const bool created = pthread_create(&thread, &attributes, run_game, nullptr) == 0;
    pthread_attr_destroy(&attributes);
    if (!created) { std::fprintf(stderr, "Badman error: cannot start the game thread\n"); exit_code = 1; }
    return created ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_org_psprecomp_badman_GameActivity_nativeSetSurface(JNIEnv *env, jclass, jobject surface,
    jint width, jint height) {
    badman::AndroidBridge::instance().set_surface(surface ? ANativeWindow_fromSurface(env, surface) : nullptr, width, height);
}
JNIEXPORT void JNICALL Java_org_psprecomp_badman_GameActivity_nativeSetPaused(JNIEnv *, jclass, jboolean paused) {
    badman::AndroidBridge::instance().set_paused(paused);
}
JNIEXPORT void JNICALL Java_org_psprecomp_badman_GameActivity_nativeSetInput(JNIEnv *, jclass, jint buttons, jint x, jint y) {
    badman::AndroidBridge::instance().set_input(static_cast<std::uint32_t>(buttons), static_cast<std::uint8_t>(x),
                                                static_cast<std::uint8_t>(y));
}
JNIEXPORT void JNICALL Java_org_psprecomp_badman_GameActivity_nativeSetSharp(JNIEnv *, jclass, jboolean sharp) {
    badman::AndroidBridge::instance().set_sharp(sharp);
}
JNIEXPORT void JNICALL Java_org_psprecomp_badman_GameActivity_nativeRequestClose(JNIEnv *, jclass) {
    badman::AndroidBridge::instance().request_close();
}
JNIEXPORT jint JNICALL Java_org_psprecomp_badman_GameActivity_nativeExitCode(JNIEnv *, jclass) { return exit_code; }
JNIEXPORT jstring JNICALL Java_org_psprecomp_badman_GameActivity_nativeUtilityMessage(JNIEnv *env, jclass) {
    return env->NewStringUTF(badman::AndroidBridge::instance().utility_message().c_str());
}
// The open keyboard dialog as {generation, description, initial text, max UTF-16 units, lines}, or null.
JNIEXPORT jobjectArray JNICALL Java_org_psprecomp_badman_GameActivity_nativeOskDialog(JNIEnv *env, jclass) {
    const auto dialog = badman::AndroidBridge::instance().osk_dialog();
    if (!dialog) return nullptr;
    auto *result = env->NewObjectArray(5, env->FindClass("java/lang/String"), nullptr);
    env->SetObjectArrayElement(result, 0, env->NewStringUTF(std::to_string(dialog->generation).c_str()));
    env->SetObjectArrayElement(result, 1, java_string(env, dialog->description));
    env->SetObjectArrayElement(result, 2, java_string(env, dialog->initial));
    env->SetObjectArrayElement(result, 3, env->NewStringUTF(std::to_string(dialog->max_units).c_str()));
    env->SetObjectArrayElement(result, 4, env->NewStringUTF(std::to_string(dialog->line_count).c_str()));
    return result;
}
JNIEXPORT void JNICALL Java_org_psprecomp_badman_GameActivity_nativeOskRespond(JNIEnv *env, jclass, jlong generation,
    jboolean confirm, jstring value) {
    badman::Osk::Response response;
    response.generation = static_cast<std::uint64_t>(generation);
    response.action = confirm ? badman::Osk::Response::Action::confirm : badman::Osk::Response::Action::cancel;
    if (value) {
        const auto *chars = env->GetStringChars(value, nullptr);
        response.text.assign(reinterpret_cast<const char16_t *>(chars), static_cast<std::size_t>(env->GetStringLength(value)));
        env->ReleaseStringChars(value, chars);
    }
    badman::AndroidBridge::instance().respond_osk(std::move(response));
}
}
