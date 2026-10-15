#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <android/hardware_buffer.h>
#include <android/surface_control.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_buffer.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifdef __cplusplus
extern "C" {
#endif

struct maton_cros_gralloc_handle;
struct MatonAhbAllocator {
  struct wlr_allocator base;
  bool has_native_handle_api;
};

struct MatonAhbBuffer {
  struct wlr_buffer base;
  AHardwareBuffer* ahb;
  const struct maton_cros_gralloc_handle* handle;
  unsigned char* staging;
  size_t stride;
  bool locked;
};

struct MatonEglUploader {
  EGLDisplay display;
  EGLContext context;
  EGLSurface surface;
  EGLImageKHR (*create_image)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer,
                              const EGLint*);
  EGLBoolean (*destroy_image)(EGLDisplay, EGLImageKHR);
  EGLClientBuffer (*get_native_client_buffer)(const AHardwareBuffer*);
  void (*image_target_texture)(GLenum, GLeglImageOES);
  bool ready;
};

bool maton_ahb_native_api_available();
bool maton_ahb_allocator_init(struct MatonAhbAllocator* allocator);
struct wlr_allocator* maton_ahb_allocator_base(struct MatonAhbAllocator* allocator);
AHardwareBuffer* maton_ahb_from_wlr_buffer(struct wlr_buffer* buffer);
bool maton_ahb_buffer_is_direct(struct wlr_buffer* buffer);
bool maton_ahb_buffer_upload_fallback(struct wlr_buffer* buffer,
                                      struct MatonEglUploader* uploader);
bool maton_egl_uploader_init(struct MatonEglUploader* uploader);
void maton_egl_uploader_finish(struct MatonEglUploader* uploader);

#ifdef __cplusplus
}
#endif
