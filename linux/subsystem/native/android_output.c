#include "android_output.h"

#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/types/wlr_buffer.h>

typedef void (*SetBufferWithRelease)(ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*,
                                     int, void*, void (*)(void*, int));
static MatonBufferReleaseDispatch release_dispatch;
static pthread_once_t set_release_once = PTHREAD_ONCE_INIT;
static SetBufferWithRelease set_buffer_with_release;

struct MatonPendingBufferRelease {
  ASurfaceControl* control;
  struct wlr_buffer* buffer;
  struct MatonPendingBufferRelease* next;
};

static void resolve_set_buffer_with_release(void) {
  set_buffer_with_release = (SetBufferWithRelease)dlsym(RTLD_DEFAULT, "ASurfaceTransaction_setBufferWithRelease");
}

static void on_buffer_release(void* opaque, int fence_fd) {
  /* Never unlock after a failed allocation/thread creation or a zero-timeout
   * poll: SurfaceFlinger may still be reading the buffer. The compositor owns
   * the fence wait and can quarantine the buffer if it cannot track the fd. */
  if (release_dispatch) release_dispatch(opaque, fence_fd);
  else if (fence_fd < 0) wlr_buffer_unlock(opaque);
  else close(fence_fd);
}

static void on_transaction_complete(void* opaque, ASurfaceTransactionStats* stats) {
  struct MatonPendingBufferRelease* pending = opaque;
  while (pending) {
    struct MatonPendingBufferRelease* next = pending->next;
    int fence_fd = ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, pending->control);
    if (release_dispatch) release_dispatch(pending->buffer, fence_fd);
    else if (fence_fd < 0) wlr_buffer_unlock(pending->buffer);
    else close(fence_fd);
    ASurfaceControl_release(pending->control);
    free(pending);
    pending = next;
  }
}

void maton_transaction_track_replaced(ASurfaceControl* control, struct wlr_buffer* replaced,
                                      struct MatonPendingBufferRelease** pending) {
  if (!control || !replaced || !pending) return;
  pthread_once(&set_release_once, resolve_set_buffer_with_release);
  if (set_buffer_with_release) return;
  struct MatonPendingBufferRelease* item = calloc(1, sizeof(*item));
  if (!item) return; /* A stale lock is safer than reusing an in-flight buffer. */
  ASurfaceControl_acquire(control);
  item->control = control;
  item->buffer = replaced;
  item->next = *pending;
  *pending = item;
}

void maton_transaction_apply(ASurfaceTransaction* transaction,
                             struct MatonPendingBufferRelease** pending) {
  if (pending && *pending) {
    ASurfaceTransaction_setOnComplete(transaction, *pending, on_transaction_complete);
    *pending = NULL; /* The callback owns this list. */
  }
  ASurfaceTransaction_apply(transaction);
}

void maton_surface_output_set_release_dispatch(MatonBufferReleaseDispatch dispatch) {
  release_dispatch = dispatch;
}

bool maton_surface_output_init(struct MatonSurfaceOutput* output, JNIEnv* env, jobject surface, const char* name) {
  if (!output || !env || !surface || !name) return false;
  output->window = ANativeWindow_fromSurface(env, surface);
  output->control = output->window ? ASurfaceControl_createFromWindow(output->window, name) : NULL;
  output->transaction = output->control ? ASurfaceTransaction_create() : NULL;
  output->shown = NULL;
  output->pending_release = NULL;
  if (!output->control || !output->transaction) {
    if (output->transaction) ASurfaceTransaction_delete(output->transaction);
    if (output->control) ASurfaceControl_release(output->control);
    if (output->window) ANativeWindow_release(output->window);
    output->window = NULL; output->control = NULL; output->transaction = NULL;
    return false;
  }
  return true;
}

void maton_surface_output_finish(struct MatonSurfaceOutput* output) {
  if (!output) return;
  if (output->transaction && output->control) {
    maton_transaction_track_replaced(output->control, output->shown, &output->pending_release);
    ASurfaceTransaction_reparent(output->transaction, output->control, NULL);
    maton_transaction_apply(output->transaction, &output->pending_release);
  }
  if (output->transaction) ASurfaceTransaction_delete(output->transaction);
  if (output->control) ASurfaceControl_release(output->control);
  if (output->window) ANativeWindow_release(output->window);
  output->transaction = NULL; output->control = NULL; output->window = NULL; output->shown = NULL;
}

void maton_transaction_set_buffer(ASurfaceTransaction* transaction, ASurfaceControl* control,
                                  AHardwareBuffer* buffer, struct wlr_buffer* base,
                                  struct wlr_buffer* replaced,
                                  struct MatonPendingBufferRelease** pending, int acquire_fence_fd) {
  pthread_once(&set_release_once, resolve_set_buffer_with_release);
  SetBufferWithRelease set_release = set_buffer_with_release;
  if (set_release && base && wlr_buffer_lock(base)) {
    set_release(transaction, control, buffer, acquire_fence_fd, base, on_buffer_release);
    return;
  }
  if (base && wlr_buffer_lock(base)) {
    ASurfaceTransaction_setBuffer(transaction, control, buffer, acquire_fence_fd);
    /* API 29–35 has no per-buffer release callback. Its completion callback
     * reports the previous buffer's release fence for each changed control. */
    maton_transaction_track_replaced(control, replaced, pending);
  } else {
    ASurfaceTransaction_setBuffer(transaction, control, buffer, acquire_fence_fd);
  }
}

bool maton_surface_output_present(struct MatonSurfaceOutput* output, AHardwareBuffer* buffer,
                                 struct wlr_buffer* base, int acquire_fence_fd) {
  if (!output || !output->control || !buffer) {
    if (acquire_fence_fd >= 0) close(acquire_fence_fd);
    return false;
  }
  if (!output->transaction) { if (acquire_fence_fd >= 0) close(acquire_fence_fd); return false; }
  maton_transaction_set_buffer(output->transaction, output->control, buffer, base, output->shown,
                               &output->pending_release, acquire_fence_fd);
  maton_transaction_apply(output->transaction, &output->pending_release);
  output->shown = base;
  return true;
}
