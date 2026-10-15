#include "android_output.h"
#include "compositor_core.h"

#include <jni.h>
#include <cstdlib>
#include <cstring>

/* NativeCompositor binds one core per process. Each stub's PerAppRuntime
 * owns its callbacks and compositor under that stub's UID. */

static JavaVM* vm;
static jobject service;
static jmethodID request_method;
static jmethodID close_method;
/* Optional (the shared host has none): window-state requests. */
static jmethodID state_method;

static JNIEnv* callback_env(bool* attached) {
  *attached = false;
  if (!vm) return nullptr;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
  if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
  *attached = true;
  return env;
}

extern "C" void maton_java_request_window(int sessionId, int id, int width, int height) {
  bool attached;
  JNIEnv* env = callback_env(&attached);
  if (env && service && request_method) {
    env->CallVoidMethod(service, request_method, sessionId, id, width, height);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  if (attached) vm->DetachCurrentThread();
}

extern "C" void maton_java_window_state(int id, int state) {
  bool attached;
  JNIEnv* env = callback_env(&attached);
  if (env && service && state_method) {
    env->CallVoidMethod(service, state_method, id, state);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  if (attached) vm->DetachCurrentThread();
}

extern "C" void maton_java_close_window(int id) {
  bool attached;
  JNIEnv* env = callback_env(&attached);
  if (env && service && close_method) {
    env->CallVoidMethod(service, close_method, id);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  if (attached) vm->DetachCurrentThread();
}

static jboolean jni_start(JNIEnv* env, jstring socket, jstring runtime, jobject owner) {
  env->GetJavaVM(&vm);
  jobject global = env->NewGlobalRef(owner);
  jclass cls = env->GetObjectClass(owner);
  jmethodID request = env->GetMethodID(cls, "onNativeToplevel", "(IIII)V");
  jmethodID close = env->GetMethodID(cls, "onNativeToplevelClosed", "(I)V");
  jmethodID state = env->GetMethodID(cls, "onNativeWindowState", "(II)V");
  if (!state && env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(cls);
  if (!global || !request || !close) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (global) env->DeleteGlobalRef(global);
    return JNI_FALSE;
  }
  service = global; request_method = request; close_method = close; state_method = state;
  const char* s = socket ? env->GetStringUTFChars(socket, nullptr) : nullptr;
  const char* r = runtime ? env->GetStringUTFChars(runtime, nullptr) : nullptr;
  bool ok = maton_core_start(s ? s : "wayland-0", r ? r : "/data/local/tmp/wayland");
  if (s) env->ReleaseStringUTFChars(socket, s);
  if (r) env->ReleaseStringUTFChars(runtime, r);
  if (!ok) {
    env->DeleteGlobalRef(service); service = nullptr; request_method = close_method = state_method = nullptr;
  }
  return ok ? JNI_TRUE : JNI_FALSE;
}

static void jni_stop(JNIEnv* env) {
  maton_core_stop();
  if (service) env->DeleteGlobalRef(service);
  service = nullptr; request_method = close_method = state_method = nullptr;
}

static jboolean jni_add_session(JNIEnv* env, jint id, jstring path) {
  if (!path) return JNI_FALSE;
  const char* value = env->GetStringUTFChars(path, nullptr);
  if (!value) return JNI_FALSE;
  bool ok = maton_core_add_session(id, value);
  env->ReleaseStringUTFChars(path, value);
  return ok ? JNI_TRUE : JNI_FALSE;
}

static jboolean jni_xwayland_init(JNIEnv* env, jstring dir, jstring path) {
  if (!dir || !path) return JNI_FALSE;
  const char* d = env->GetStringUTFChars(dir, nullptr);
  const char* p = env->GetStringUTFChars(path, nullptr);
  bool ok = d && p && maton_core_xwayland_init(d, p);
  if (d) env->ReleaseStringUTFChars(dir, d);
  if (p) env->ReleaseStringUTFChars(path, p);
  return ok ? JNI_TRUE : JNI_FALSE;
}

static jstring jni_add_xwayland(JNIEnv* env, jint session, jint uid) {
  char display[128];
  if (!maton_core_add_xwayland(session, uid, display, sizeof(display))) return nullptr;
  return env->NewStringUTF(display);
}

static void jni_attach(JNIEnv* env, jint id, jobject surface, jint width, jint height) {
  MatonSurfaceOutput* output = static_cast<MatonSurfaceOutput*>(calloc(1, sizeof(*output)));
  if (!output || !maton_surface_output_init(output, env, surface, "maton-wayland-window")) {
    free(output); return;
  }
  maton_core_attach(id, output, width, height);
}

/* ---- Per-app NativeCompositor binding ---- */

/* ---- org.matonos.linux.runtime.NativeCompositor (per-app) ---- */

extern "C" JNIEXPORT jboolean JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeStart(JNIEnv* env, jclass, jstring socket,
                                                                 jstring runtime, jobject owner) {
  return jni_start(env, socket, runtime, owner);
}
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeStop(JNIEnv* env, jclass) { jni_stop(env); }
extern "C" JNIEXPORT jboolean JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeAddSession(JNIEnv* env, jclass, jint id, jstring path) {
  return jni_add_session(env, id, path);
}
extern "C" JNIEXPORT jboolean JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeXwaylandInit(JNIEnv* env, jclass, jstring dir, jstring path) {
  return jni_xwayland_init(env, dir, path);
}
extern "C" JNIEXPORT jstring JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeAddXwayland(JNIEnv* env, jclass, jint session, jint uid) {
  return jni_add_xwayland(env, session, uid);
}
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeLaunchDemo(JNIEnv*, jclass) { maton_core_launch_demo(); }
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeAttach(JNIEnv* env, jclass, jint id, jobject surface, jint w, jint h) {
  jni_attach(env, id, surface, w, h);
}
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeDetach(JNIEnv*, jclass, jint id) { maton_core_detach(id); }
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeResize(JNIEnv*, jclass, jint id, jint w, jint h) { maton_core_resize(id,w,h); }
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeSetFullscreen(JNIEnv*, jclass, jint id, jboolean on) { maton_core_set_fullscreen(id,on?1:0); }
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeKey(JNIEnv*, jclass, jint id, jint key, jint scan, jint action, jint meta, jlong time) {
  maton_core_key(id,key,scan,action,meta,time);
}
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeMotion(JNIEnv*, jclass, jint id, jfloat x, jfloat y, jfloat vs, jfloat hs, jint action, jint buttons, jlong time) {
  maton_core_motion(id,x,y,vs,hs,action,buttons,time);
}
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeStopped(JNIEnv*, jclass, jint id, jboolean stopped) { maton_core_stopped(id,stopped); }
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeClose(JNIEnv*, jclass, jint id) { maton_core_close(id); }

extern "C" JNIEXPORT void JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeCaptureLost(JNIEnv*,jclass,jint id){maton_core_capture_lost(id);}
extern "C" JNIEXPORT void JNICALL
Java_org_matonos_linux_runtime_NativeCompositor_nativeRelative(JNIEnv*,jclass,jint id,jfloat x,jfloat y,jfloat vs,jfloat hs,jint buttons,jlong time){maton_core_relative(id,x,y,vs,hs,buttons,time);}
