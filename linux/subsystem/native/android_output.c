#include "android_output.h"

#include <android/native_window_jni.h>
#include <unistd.h>
#include <wlr/types/wlr_buffer.h>

static MatonBufferReleaseDispatch release_dispatch;

static void on_buffer_release(void* opaque, int fence_fd) {
  /* Never unlock after a failed allocation/thread creation or a zero-timeout
   * poll: SurfaceFlinger may still be reading the buffer. The compositor owns
   * the fence wait and can quarantine the buffer if it cannot track the fd. */
  if (release_dispatch) release_dispatch(opaque, fence_fd);
  else if (fence_fd < 0) wlr_buffer_unlock(opaque);
  else close(fence_fd);
}

void maton_transaction_apply(ASurfaceTransaction* transaction) {
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
    ASurfaceTransaction_reparent(output->transaction, output->control, NULL);
    maton_transaction_apply(output->transaction);
  }
  if (output->transaction) ASurfaceTransaction_delete(output->transaction);
  if (output->control) ASurfaceControl_release(output->control);
  if (output->window) ANativeWindow_release(output->window);
  output->transaction = NULL; output->control = NULL; output->window = NULL; output->shown = NULL;
}

void maton_transaction_set_buffer(ASurfaceTransaction* transaction, ASurfaceControl* control,
                                  AHardwareBuffer* buffer, struct wlr_buffer* base,
                                  int acquire_fence_fd) {
  if (base && wlr_buffer_lock(base)) {
    /* API 36's buffer-release callback is associated with the actual buffer
     * lifetime. Transaction completion callbacks create a new IRemoteCallback
     * Binder for every changed frame and can exhaust system_server's quota. */
    ASurfaceTransaction_setBufferWithRelease(transaction, control, buffer, acquire_fence_fd,
                                             base, on_buffer_release);
    return;
  }
  ASurfaceTransaction_setBuffer(transaction, control, buffer, acquire_fence_fd);
}

bool maton_surface_output_present(struct MatonSurfaceOutput* output, AHardwareBuffer* buffer,
                                 struct wlr_buffer* base, int acquire_fence_fd) {
  if (!output || !output->control || !buffer) {
    if (acquire_fence_fd >= 0) close(acquire_fence_fd);
    return false;
  }
  if (!output->transaction) { if (acquire_fence_fd >= 0) close(acquire_fence_fd); return false; }
  maton_transaction_set_buffer(output->transaction, output->control, buffer, base, acquire_fence_fd);
  maton_transaction_apply(output->transaction);
  output->shown = base;
  return true;
}
