#pragma once

#include <android/native_window.h>
#include <android/surface_control.h>
#include <jni.h>
#include <stdbool.h>

struct wlr_buffer;
struct wlr_surface;
struct MatonPendingBufferRelease;
/* Takes ownership of fence_fd. Negative means there is no outstanding reader. */
typedef void (*MatonBufferReleaseDispatch)(struct wlr_buffer*, int fence_fd);

typedef struct MatonSurfaceOutput {
  ANativeWindow* window;
  ASurfaceControl* control;
  ASurfaceTransaction* transaction;
  struct wlr_buffer* shown;
  struct MatonPendingBufferRelease* pending_release;
} MatonSurfaceOutput;

#ifdef __cplusplus
extern "C" {
#endif
bool maton_surface_output_init(struct MatonSurfaceOutput* output, JNIEnv* env, jobject surface, const char* name);
void maton_surface_output_finish(struct MatonSurfaceOutput* output);
bool maton_surface_output_present(struct MatonSurfaceOutput* output, AHardwareBuffer* buffer,
                                  struct wlr_buffer* buffer_base, int acquire_fence_fd);
void maton_surface_output_set_release_dispatch(MatonBufferReleaseDispatch dispatch);
int maton_sync_take_acquire(struct wlr_surface* surface);
/* Sets buffer on control in transaction. base (optional) remains locked
 * until SurfaceFlinger releases it and its release fence signals. */
void maton_transaction_set_buffer(ASurfaceTransaction* transaction, ASurfaceControl* control,
                                  AHardwareBuffer* buffer, struct wlr_buffer* base,
                                  struct wlr_buffer* replaced,
                                  struct MatonPendingBufferRelease** pending, int acquire_fence_fd);
void maton_transaction_track_replaced(ASurfaceControl* control, struct wlr_buffer* replaced,
                                      struct MatonPendingBufferRelease** pending);
void maton_transaction_apply(ASurfaceTransaction* transaction,
                             struct MatonPendingBufferRelease** pending);
#ifdef __cplusplus
}
#endif
