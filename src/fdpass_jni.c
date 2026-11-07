/*
 * fdpass_jni.c — 唯一依赖 JNI 的薄层。
 *
 * 导出符号必须与原 libfdpass.so 完全一致（nm -D 取证，只有这 3 个 T 符号）：
 *   Java_com_Android_MoFlash_data_FdPass_sendFd
 *   Java_com_Android_MoFlash_data_FdPass_serveFdLoop
 *   Java_com_Android_MoFlash_data_FdPass_stopServeLoop
 *
 * 使用短名静态解析（无 JNI_OnLoad、无 RegisterNatives），与 Kotlin 侧
 *   class FdPass { external fun sendFd(fd: Int, name: String?): Int ... }
 * 的调用方式完全对应，因此应用侧不需要改任何一行 Kotlin/Java 代码。
 *
 * 编译时加 -fvisibility=hidden，仅这 3 个 JNIEXPORT 函数导出，避免符号污染。
 */
#include <jni.h>
#include "fdpass_core.h"

/* NDK 的 JNIEXPORT 即 visibility("default")；host JDK jni.h 同理 */
#ifndef JNIEXPORT
#define JNIEXPORT __attribute__((visibility("default")))
#endif

typedef int (*fdpass_fn)(int fd, const char *name);

static jint call_with_name(JNIEnv *env, jstring name, jint fd, fdpass_fn fn) {
    if (name == NULL) return (jint)FDPASS_ERR_NAME;
    const char *cname = (*env)->GetStringUTFChars(env, name, NULL);
    if (cname == NULL) return (jint)FDPASS_ERR_NAME;
    jint ret = (jint)fn((int)fd, cname);
    (*env)->ReleaseStringUTFChars(env, name, cname);
    return ret;
}

JNIEXPORT jint JNICALL
Java_com_Android_MoFlash_data_FdPass_serveFdLoop(JNIEnv *env, jobject thiz,
                                                 jint fd, jstring name) {
    (void)thiz;
    return call_with_name(env, name, fd, fdpass_serve_loop);
}

JNIEXPORT jint JNICALL
Java_com_Android_MoFlash_data_FdPass_sendFd(JNIEnv *env, jobject thiz,
                                            jint fd, jstring name) {
    (void)thiz;
    return call_with_name(env, name, fd, fdpass_send_once);
}

JNIEXPORT void JNICALL
Java_com_Android_MoFlash_data_FdPass_stopServeLoop(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    fdpass_stop_serve_loop();
}
