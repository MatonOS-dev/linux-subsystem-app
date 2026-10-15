#include "android_buffer.h"
#include "gralloc_handle.h"

#include <android/hardware_buffer.h>
#include <cutils/native_handle.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/drm_format_set.h>
#include <drm/drm_fourcc.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t k_cros_gralloc_magic = MATON_CROS_GRALLOC_MAGIC;
#define DRV_MAX_PLANES MATON_DRV_MAX_PLANES
typedef const native_handle_t* (*GetNativeHandle)(const AHardwareBuffer*);
typedef int (*CreateFromHandle)(const AHardwareBuffer_Desc*, const native_handle_t*, int32_t, AHardwareBuffer**);
static GetNativeHandle get_native_handle;
static CreateFromHandle create_from_handle;
static void* nativewindow_handle;

static struct MatonAhbBuffer* as_buffer(struct wlr_buffer* buffer) {
  return (struct MatonAhbBuffer*)buffer;
}

static void destroy_buffer(struct wlr_buffer* base) {
  struct MatonAhbBuffer* buffer = as_buffer(base);
  if (buffer->locked) AHardwareBuffer_unlock(buffer->ahb, NULL);
  if (buffer->ahb) AHardwareBuffer_release(buffer->ahb);
  free(buffer->staging);
  wlr_buffer_finish(base);
  free(buffer);
}

static bool get_dmabuf(struct wlr_buffer* base, struct wlr_dmabuf_attributes* out) {
  struct MatonAhbBuffer* buffer = as_buffer(base);
  const struct maton_cros_gralloc_handle* handle = buffer->handle;
  if (!handle || handle->magic != k_cros_gralloc_magic || handle->num_planes == 0 ||
      handle->num_planes > WLR_DMABUF_MAX_PLANES || handle->numFds < (int)handle->num_planes ||
      handle->width != (uint32_t)base->width || handle->height != (uint32_t)base->height) return false;
  memset(out, 0, sizeof(*out));
  out->width = base->width; out->height = base->height;
  out->format = handle->format; out->modifier = handle->format_modifier;
  out->n_planes = (int)handle->num_planes;
  for (int i = 0; i < out->n_planes; ++i) {
    if (handle->fds[i] < 0 || handle->strides[i] == 0) return false;
    out->fd[i] = handle->fds[i];  /* Borrowed from the AHardwareBuffer-owned handle. */
    out->offset[i] = handle->offsets[i];
    out->stride[i] = handle->strides[i];
  }
  return true;
}

static bool begin_access(struct wlr_buffer* base, uint32_t flags, void** data,
                         uint32_t* format, size_t* stride) {
  struct MatonAhbBuffer* buffer = as_buffer(base);
  if (!(flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) || buffer->locked) return false;
  if (buffer->staging) {
    *data = buffer->staging; *format = DRM_FORMAT_ARGB8888; *stride = buffer->stride;
    return true;
  }
  void* pixels = NULL;
  uint64_t usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
  if (AHardwareBuffer_lock(buffer->ahb, usage, -1, NULL, &pixels) != 0 || !pixels) return false;
  buffer->locked = true; *data = pixels;
  *format = buffer->handle ? buffer->handle->format : DRM_FORMAT_ARGB8888;
  *stride = buffer->handle && buffer->handle->strides[0] ? buffer->handle->strides[0] : buffer->stride;
  return true;
}

static void end_access(struct wlr_buffer* base) {
  struct MatonAhbBuffer* buffer = as_buffer(base);
  if (buffer->staging) return;
  if (buffer->locked) { AHardwareBuffer_unlock(buffer->ahb, NULL); buffer->locked = false; }
}

static const struct wlr_buffer_impl buffer_impl = {
  .destroy = destroy_buffer,
  .get_dmabuf = get_dmabuf,
  .get_shm = NULL,
  .begin_data_ptr_access = begin_access,
  .end_data_ptr_access = end_access,
};

static void destroy_allocator(struct wlr_allocator* base) {
  wl_signal_emit_mutable(&base->events.destroy, NULL);
}

static struct wlr_buffer* create_buffer(struct wlr_allocator* base, int width, int height,
                                       const struct wlr_drm_format* format) {
  struct MatonAhbAllocator* allocator = (struct MatonAhbAllocator*)base;
  if (width <= 0 || height <= 0 || !format || format->format != DRM_FORMAT_ARGB8888) return NULL;
  struct MatonAhbBuffer* buffer = calloc(1, sizeof(*buffer));
  if (!buffer) return NULL;
  AHardwareBuffer_Desc desc = {0};
  desc.width = (uint32_t)width; desc.height = (uint32_t)height; desc.layers = 1;
  /* Platform BGRA format value; the public NDK header omits its symbolic name. */
  desc.format = 5;
  desc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
      AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
  if (AHardwareBuffer_allocate(&desc, &buffer->ahb) != 0 || !buffer->ahb) { free(buffer); return NULL; }
  AHardwareBuffer_Desc actual = {0}; AHardwareBuffer_describe(buffer->ahb, &actual);
  buffer->stride = (size_t)actual.stride * 4;
  if (allocator->has_native_handle_api && get_native_handle) {
    const native_handle_t* native = get_native_handle(buffer->ahb);
    const struct maton_cros_gralloc_handle* gralloc = (const struct maton_cros_gralloc_handle*)native;
    if (gralloc && gralloc->magic == k_cros_gralloc_magic && gralloc->num_planes > 0 &&
        gralloc->num_planes <= DRV_MAX_PLANES && gralloc->numFds >= (int)gralloc->num_planes &&
        gralloc->width == desc.width && gralloc->height == desc.height && gralloc->droid_format == (int32_t)desc.format) {
      buffer->handle = gralloc;
    }
  }
  if (!buffer->handle) {
    buffer->staging = calloc((size_t)height, buffer->stride);
    if (!buffer->staging) { AHardwareBuffer_release(buffer->ahb); free(buffer); return NULL; }
  }
  wlr_buffer_init(&buffer->base, &buffer_impl, width, height);
  return &buffer->base;
}

static const struct wlr_allocator_interface allocator_impl = {
  .create_buffer = create_buffer,
  .destroy = destroy_allocator,
};

bool maton_ahb_native_api_available(void) {
  if (!nativewindow_handle) {
    nativewindow_handle = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
    if (!nativewindow_handle) return false;
    get_native_handle = (GetNativeHandle)dlsym(nativewindow_handle, "AHardwareBuffer_getNativeHandle");
    create_from_handle = (CreateFromHandle)dlsym(nativewindow_handle, "AHardwareBuffer_createFromHandle");
  }
  return get_native_handle != NULL;
}

bool maton_ahb_allocator_init(struct MatonAhbAllocator* allocator) {
  if (!allocator) return false;
  allocator->has_native_handle_api = maton_ahb_native_api_available();
  wlr_allocator_init(&allocator->base, &allocator_impl, WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_DATA_PTR);
  return true;
}

struct wlr_allocator* maton_ahb_allocator_base(struct MatonAhbAllocator* allocator) {
  return allocator ? &allocator->base : NULL;
}

AHardwareBuffer* maton_ahb_from_wlr_buffer(struct wlr_buffer* buffer) {
  return buffer && buffer->impl == &buffer_impl ? as_buffer(buffer)->ahb : NULL;
}

bool maton_ahb_buffer_is_direct(struct wlr_buffer* buffer) {
  return buffer && buffer->impl == &buffer_impl && as_buffer(buffer)->handle != NULL;
}

bool maton_ahb_buffer_upload_fallback(struct wlr_buffer* base, struct MatonEglUploader* uploader) {
  if (!base || base->impl != &buffer_impl || !uploader || !uploader->ready || !uploader->get_native_client_buffer ||
      !uploader->create_image || !as_buffer(base)->staging) return false;
  struct MatonAhbBuffer* buffer = as_buffer(base);
  EGLClientBuffer client = uploader->get_native_client_buffer(buffer->ahb);
  if (!client) return false;
  const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
  EGLImageKHR image = uploader->create_image(uploader->display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client, attrs);
  if (image == EGL_NO_IMAGE_KHR) return false;
  GLuint texture = 0; glGenTextures(1, &texture);
  if (texture == 0) { uploader->destroy_image(uploader->display, image); return false; }
  glBindTexture(GL_TEXTURE_2D, texture);
  if (uploader->image_target_texture) uploader->image_target_texture(GL_TEXTURE_2D, image);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, base->width, base->height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, buffer->staging);
  GLenum error = glGetError(); glFinish();
  if (uploader->destroy_image) uploader->destroy_image(uploader->display, image);
  glDeleteTextures(1, &texture);
  return error == GL_NO_ERROR;
}

bool maton_egl_uploader_init(struct MatonEglUploader* uploader) {
  if (!uploader) return false;
  memset(uploader, 0, sizeof(*uploader));
  uploader->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (uploader->display == EGL_NO_DISPLAY || !eglInitialize(uploader->display, NULL, NULL)) return false;
  const EGLint config_attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLConfig config = NULL; EGLint count = 0;
  if (!eglChooseConfig(uploader->display, config_attrs, &config, 1, &count) || count != 1) return false;
  const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  uploader->context = eglCreateContext(uploader->display, config, EGL_NO_CONTEXT, context_attrs);
  const EGLint surface_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  uploader->surface = eglCreatePbufferSurface(uploader->display, config, surface_attrs);
  if (uploader->context == EGL_NO_CONTEXT || uploader->surface == EGL_NO_SURFACE ||
      !eglMakeCurrent(uploader->display, uploader->surface, uploader->surface, uploader->context)) return false;
  uploader->create_image = (EGLImageKHR (*)(EGLDisplay,EGLContext,EGLenum,EGLClientBuffer,const EGLint*))eglGetProcAddress("eglCreateImageKHR");
  uploader->destroy_image = (EGLBoolean (*)(EGLDisplay,EGLImageKHR))eglGetProcAddress("eglDestroyImageKHR");
  uploader->get_native_client_buffer = (EGLClientBuffer (*)(const AHardwareBuffer*))eglGetProcAddress("eglGetNativeClientBufferANDROID");
  uploader->image_target_texture = (void (*)(GLenum,GLeglImageOES))eglGetProcAddress("glEGLImageTargetTexture2DOES");
  uploader->ready = uploader->create_image && uploader->destroy_image &&
      uploader->get_native_client_buffer && uploader->image_target_texture;
  return uploader->ready;
}

void maton_egl_uploader_finish(struct MatonEglUploader* uploader) {
  if (!uploader) return;
  if (uploader->display != EGL_NO_DISPLAY) {
    eglMakeCurrent(uploader->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (uploader->surface != EGL_NO_SURFACE) eglDestroySurface(uploader->display, uploader->surface);
    if (uploader->context != EGL_NO_CONTEXT) eglDestroyContext(uploader->display, uploader->context);
    eglTerminate(uploader->display);
  }
  memset(uploader, 0, sizeof(*uploader));
}
#include <stdlib.h>
#include <string.h>
