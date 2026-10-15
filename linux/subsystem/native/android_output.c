#include "android_output.h"

#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/types/wlr_buffer.h>

typedef void (*SetBufferWithRelease)(ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*,
                                     int, void*, void (*)(void*, int));
static MatonBufferReleaseDispatch release_dispatch;
static pthread_once_t set_release_once = PTHREAD_ONCE_INIT;
static SetBufferWithRelease set_buffer_with_release;

static void resolve_set_buffer_with_release(void) {
  set_buffer_with_release = (SetBufferWithRelease)dlsym(RTLD_DEFAULT, "ASurfaceTransaction_setBufferWithRelease");
}

struct ReleaseWait { struct wlr_buffer* buffer; int fence; };

static void* wait_release(void* opaque) {
  struct ReleaseWait* wait = opaque;
  if (wait->fence >= 0) {
    struct pollfd pfd = { .fd = wait->fence, .events = POLLIN };
    while (poll(&pfd, 1, -1) < 0 && errno == EINTR) {}
    close(wait->fence);
  }
  if (release_dispatch) release_dispatch(wait->buffer);
  else wlr_buffer_unlock(wait->buffer);
  free(wait);
  return NULL;
}

static void on_buffer_release(void* opaque, int fence_fd) {
  struct ReleaseWait* wait = malloc(sizeof(*wait));
  if (!wait) {
    if (fence_fd >= 0) { struct pollfd pfd = { .fd=fence_fd, .events=POLLIN }; (void)poll(&pfd,1,0); close(fence_fd); }
    if (release_dispatch) release_dispatch(opaque); else wlr_buffer_unlock(opaque);
    return;
  }
  wait->buffer = opaque; wait->fence = fence_fd;
  pthread_t thread;
  if (pthread_create(&thread, NULL, wait_release, wait) == 0) pthread_detach(thread);
  else {
    if (wait->fence >= 0) { struct pollfd pfd = { .fd=wait->fence, .events=POLLIN }; (void)poll(&pfd,1,0); close(wait->fence); }
    if (release_dispatch) release_dispatch(wait->buffer); else wlr_buffer_unlock(wait->buffer);
    free(wait);
  }
}

void maton_surface_output_set_release_dispatch(MatonBufferReleaseDispatch dispatch) {
  release_dispatch = dispatch;
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

void maton_transaction_set_buffer(ASurfaceTransaction* transaction, ASurfaceControl* control,
                                  AHardwareBuffer* buffer, struct wlr_buffer* base, int acquire_fence_fd) {
  pthread_once(&set_release_once, resolve_set_buffer_with_release);
  SetBufferWithRelease set_release = set_buffer_with_release;
  if (set_release && base && wlr_buffer_lock(base))
    set_release(transaction, control, buffer, acquire_fence_fd, base, on_buffer_release);
  else
    ASurfaceTransaction_setBuffer(transaction, control, buffer, acquire_fence_fd);
}

bool maton_surface_output_present(struct MatonSurfaceOutput* output, AHardwareBuffer* buffer,
                                 struct wlr_buffer* base, int acquire_fence_fd) {
  if (!output || !output->control || !buffer) {
    if (acquire_fence_fd >= 0) close(acquire_fence_fd);
    return false;
  }
  ASurfaceTransaction* transaction = ASurfaceTransaction_create();
  if (!transaction) { if (acquire_fence_fd >= 0) close(acquire_fence_fd); return false; }
  maton_transaction_set_buffer(transaction, output->control, buffer, base, acquire_fence_fd);
  ASurfaceTransaction_apply(transaction);
  ASurfaceTransaction_delete(transaction);
  return true;
}
