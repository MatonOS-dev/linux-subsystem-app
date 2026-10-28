#!/usr/bin/env python3
"""Exercise the production buffer-release path with fake SurfaceControl APIs."""

from pathlib import Path
import subprocess
import tempfile


native = Path(__file__).resolve().parents[1]
source = native / "android_output.c"

headers = {
    "jni.h": r"""
#ifndef MATON_TEST_JNI_H
#define MATON_TEST_JNI_H
typedef void JNIEnv;
typedef void* jobject;
#endif
""",
    "android/native_window.h": r"""
#ifndef MATON_TEST_NATIVE_WINDOW_H
#define MATON_TEST_NATIVE_WINDOW_H
typedef struct ANativeWindow ANativeWindow;
#endif
""",
    "android/native_window_jni.h": r"""
#include <jni.h>
#include <android/native_window.h>
ANativeWindow* ANativeWindow_fromSurface(JNIEnv*, jobject);
void ANativeWindow_release(ANativeWindow*);
""",
    "android/surface_control.h": r"""
#ifndef MATON_TEST_SURFACE_CONTROL_H
#define MATON_TEST_SURFACE_CONTROL_H
#include <android/native_window.h>
typedef struct ASurfaceControl ASurfaceControl;
typedef struct ASurfaceTransaction ASurfaceTransaction;
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct ASurfaceTransactionStats ASurfaceTransactionStats;
typedef void (*ASurfaceTransaction_OnBufferRelease)(void*, int);
typedef void (*ASurfaceTransaction_OnComplete)(void*, ASurfaceTransactionStats*);
ASurfaceControl* ASurfaceControl_create(ASurfaceControl*, const char*);
ASurfaceControl* ASurfaceControl_createFromWindow(ANativeWindow*, const char*);
void ASurfaceControl_release(ASurfaceControl*);
ASurfaceTransaction* ASurfaceTransaction_create(void);
void ASurfaceTransaction_delete(ASurfaceTransaction*);
void ASurfaceTransaction_apply(ASurfaceTransaction*);
void ASurfaceTransaction_reparent(ASurfaceTransaction*, ASurfaceControl*, ASurfaceControl*);
void ASurfaceTransaction_setBufferWithRelease(ASurfaceTransaction*, ASurfaceControl*,
    AHardwareBuffer*, int, void*, ASurfaceTransaction_OnBufferRelease);
void ASurfaceTransaction_setBuffer(ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*, int);
void ASurfaceTransaction_setOnComplete(ASurfaceTransaction*, void*, ASurfaceTransaction_OnComplete);
#endif
""",
    "wlr/types/wlr_buffer.h": r"""
#ifndef MATON_TEST_WLR_BUFFER_H
#define MATON_TEST_WLR_BUFFER_H
struct wlr_buffer { unsigned locks; };
bool wlr_buffer_lock(struct wlr_buffer*);
void wlr_buffer_unlock(struct wlr_buffer*);
#endif
""",
}

test = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <jni.h>
#include <android/native_window.h>
#include <android/surface_control.h>
#include <wlr/types/wlr_buffer.h>

struct ANativeWindow { int unused; };
struct ASurfaceControl { int unused; };
struct ASurfaceTransaction { int unused; };
struct AHardwareBuffer { int unused; };
struct ASurfaceTransactionStats { int unused; };
static unsigned applied, release_callbacks, binder_callbacks, fallback_buffers;
static ASurfaceTransaction_OnBufferRelease current_release;
static void* current_context;

bool wlr_buffer_lock(struct wlr_buffer* b) { ++b->locks; return true; }
void wlr_buffer_unlock(struct wlr_buffer* b) { assert(b->locks); --b->locks; }
ANativeWindow* ANativeWindow_fromSurface(JNIEnv* env, jobject surface) {
  (void)env; (void)surface; return calloc(1, sizeof(ANativeWindow));
}
void ANativeWindow_release(ANativeWindow* w) { free(w); }
ASurfaceControl* ASurfaceControl_create(ASurfaceControl* parent, const char* name) {
  (void)parent; (void)name; return calloc(1, sizeof(ASurfaceControl));
}
ASurfaceControl* ASurfaceControl_createFromWindow(ANativeWindow* w, const char* name) {
  (void)w; (void)name; return calloc(1, sizeof(ASurfaceControl));
}
void ASurfaceControl_release(ASurfaceControl* c) { free(c); }
ASurfaceTransaction* ASurfaceTransaction_create(void) { return calloc(1, sizeof(ASurfaceTransaction)); }
void ASurfaceTransaction_delete(ASurfaceTransaction* t) { free(t); }
void ASurfaceTransaction_apply(ASurfaceTransaction* t) { (void)t; ++applied; }
void ASurfaceTransaction_reparent(ASurfaceTransaction* t, ASurfaceControl* c, ASurfaceControl* p) {
  (void)t; (void)c; (void)p;
}
void ASurfaceTransaction_setBufferWithRelease(ASurfaceTransaction* t, ASurfaceControl* c,
    AHardwareBuffer* b, int fence, void* context, ASurfaceTransaction_OnBufferRelease callback) {
  (void)t; (void)c; (void)b; (void)fence;
  if (current_release) { current_release(current_context, -1); ++release_callbacks; }
  current_context = context;
  current_release = callback;
}
void ASurfaceTransaction_setBuffer(ASurfaceTransaction* t, ASurfaceControl* c,
    AHardwareBuffer* b, int fence) { (void)t; (void)c; (void)b; (void)fence; ++fallback_buffers; }
void ASurfaceTransaction_setOnComplete(ASurfaceTransaction* t, void* context,
    ASurfaceTransaction_OnComplete callback) {
  (void)t; (void)context; (void)callback; ++binder_callbacks;
}

#include "android_output.c"

int main(void) {
  ASurfaceTransaction transaction = {0};
  ASurfaceControl control = {0};
  AHardwareBuffer hardware_buffer = {0};
  struct wlr_buffer buffers[3] = {{0}, {0}, {0}};
  for (unsigned i = 0; i < 10000; ++i) {
    struct wlr_buffer* buffer = &buffers[i % 3];
    maton_transaction_set_buffer(&transaction, &control, &hardware_buffer, buffer, -1);
    maton_transaction_apply(&transaction);
  }
  assert(current_release);
  current_release(current_context, -1);
  ++release_callbacks;
  current_release = NULL;
  assert(applied == 10000);
  assert(release_callbacks == 10000);
  assert(binder_callbacks == 0);
  assert(fallback_buffers == 0);
  for (unsigned i = 0; i < 3; ++i) assert(buffers[i].locks == 0);
  puts("PASS 10k production buffer updates use per-buffer releases and create 0 transaction Binder callbacks");
  return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="maton-release-callback-") as temp:
    temp = Path(temp)
    for relative, content in headers.items():
        header = temp / relative
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text(content)
    harness = temp / "release-callback-test.c"
    harness.write_text(test)
    binary = temp / "release-callback-test"
    subprocess.run([
        "cc", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(temp),
        "-I", str(native), "-include", "stdbool.h", str(harness), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
