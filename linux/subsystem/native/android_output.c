#include "android_output.h"
#include "buffer_release.h"

#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <wlr/types/wlr_buffer.h>

typedef void (*SetBufferWithRelease)(ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*,
                                     int, void*, void (*)(void*, int));
static pthread_once_t set_release_once = PTHREAD_ONCE_INIT;
static SetBufferWithRelease set_buffer_with_release;
static void resolve_set_buffer_with_release(void) {
  set_buffer_with_release = (SetBufferWithRelease)dlsym(RTLD_DEFAULT, "ASurfaceTransaction_setBufferWithRelease");
}

bool maton_surface_output_has_release(void) {
  pthread_once(&set_release_once, resolve_set_buffer_with_release);
  return set_buffer_with_release != NULL;
}

bool maton_surface_output_init(struct MatonSurfaceOutput* output, JNIEnv* env, jobject surface, const char* name) {
  if (!output || !env || !surface || !name) return false;
  output->window = ANativeWindow_fromSurface(env, surface);
  output->control = output->window ? ASurfaceControl_createFromWindow(output->window, name) : NULL;
  if (!output->control) {
    if (output->window) ANativeWindow_release(output->window);
    output->window = NULL;
    return false;
  }
  return true;
}

void maton_surface_output_finish(struct MatonSurfaceOutput* output) {
  if (!output) return;
  if (output->control) ASurfaceControl_release(output->control);
  if (output->window) ANativeWindow_release(output->window);
  output->control = NULL; output->window = NULL;
}

bool maton_transaction_set_buffer(ASurfaceTransaction* transaction, ASurfaceControl* control,
                                  AHardwareBuffer* buffer, struct wlr_buffer* base, int acquire_fence_fd) {
  pthread_once(&set_release_once, resolve_set_buffer_with_release);
  /* Reserve tracking before Android can read the buffer. Missing release API,
   * OOM or saturation leaves the old image in place and lets the caller retry. */
  struct MatonBufferRelease* release = base && set_buffer_with_release
      ? maton_buffer_release_reserve(base) : NULL;
  if (!release) { if (acquire_fence_fd >= 0) close(acquire_fence_fd); return false; }
  if (!wlr_buffer_lock(base)) {
    maton_buffer_release_cancel(release);
    if (acquire_fence_fd >= 0) close(acquire_fence_fd);
    return false;
  }
  set_buffer_with_release(transaction, control, buffer, acquire_fence_fd, release,
                           maton_buffer_release_complete);
  return true;
}

bool maton_surface_output_present(struct MatonSurfaceOutput* output, AHardwareBuffer* buffer,
                                 struct wlr_buffer* base, int acquire_fence_fd) {
  if (!output || !output->control || !buffer) {
    if (acquire_fence_fd >= 0) close(acquire_fence_fd);
    return false;
  }
  ASurfaceTransaction* transaction = ASurfaceTransaction_create();
  if (!transaction) { if (acquire_fence_fd >= 0) close(acquire_fence_fd); return false; }
  bool submitted = maton_transaction_set_buffer(transaction, output->control, buffer, base, acquire_fence_fd);
  ASurfaceTransaction_apply(transaction);
  ASurfaceTransaction_delete(transaction);
  return submitted;
}
