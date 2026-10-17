#pragma once

#include <android/native_window.h>
#include <android/surface_control.h>
#include <jni.h>
#include <stdbool.h>

struct wlr_buffer;

typedef struct MatonSurfaceOutput {
  ANativeWindow* window;
  ASurfaceControl* control;
} MatonSurfaceOutput;

#ifdef __cplusplus
extern "C" {
#endif
bool maton_surface_output_has_release(void);
bool maton_surface_output_init(struct MatonSurfaceOutput* output, JNIEnv* env, jobject surface, const char* name);
void maton_surface_output_finish(struct MatonSurfaceOutput* output);
bool maton_surface_output_present(struct MatonSurfaceOutput* output, AHardwareBuffer* buffer,
                                  struct wlr_buffer* buffer_base, int acquire_fence_fd);
/* Takes acquire_fence_fd on success AND failure. Returns false before submission
 * if release tracking is unavailable/full. base (required) is locked until
 * SurfaceFlinger releases the buffer, then unlocked on the Wayland event loop. */
bool maton_transaction_set_buffer(ASurfaceTransaction* transaction, ASurfaceControl* control,
                                  AHardwareBuffer* buffer, struct wlr_buffer* base, int acquire_fence_fd);
#ifdef __cplusplus
}
#endif
