#include "dmabuf_import.h"
#include "gralloc_handle.h"

#include <android/log.h>
#include <dlfcn.h>
#include <drm/drm_fourcc.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/addon.h>

/* Client dma-bufs reach SurfaceFlinger as AHardwareBuffers. gralloc can only
 * import its own handle type, so the dma-buf is wrapped in a cros_gralloc
 * handle with id 0: the MatonOS minigbm fork treats id 0 as an external
 * buffer, keyed by the dma-buf's identity, with process-local metadata. */

/* vndk/hardware_buffer.h and minigbm drv.h / gralloc usage bits. */
#define CREATE_FROM_HANDLE_METHOD_CLONE 3
#define BO_USE_TEXTURE (1ull << 5)
#define GRALLOC_USAGE_HW_TEXTURE 0x00000100
#define GRALLOC_USAGE_HW_COMPOSER 0x00000800

typedef int (*CreateFromHandle)(const AHardwareBuffer_Desc*, const native_handle_t*, int32_t, AHardwareBuffer**);
typedef const native_handle_t* (*GetNativeHandle)(const AHardwareBuffer*);
static GetNativeHandle get_native_handle;
static CreateFromHandle create_from_handle;
extern int32_t maton_dmabuf_plane_metadata(const native_handle_t*, void*, size_t);
static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static void resolve(void) {
  void* lib = dlopen("libnativewindow.so", RTLD_NOW);
  if (lib) {
    create_from_handle = (CreateFromHandle)dlsym(lib, "AHardwareBuffer_createFromHandle");
    get_native_handle = (GetNativeHandle)dlsym(lib, "AHardwareBuffer_getNativeHandle");
  }
}

struct Format { uint32_t drm; uint32_t ahb; uint32_t bytes; bool opaque; };
/* DRM fourccs name channels from the most significant bit: ABGR8888 is
 * R,G,B,A in memory, Android's RGBA_8888. */
static const struct Format formats[] = {
  { DRM_FORMAT_ABGR8888, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 4, false },
  { DRM_FORMAT_XBGR8888, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 4, true },
  { DRM_FORMAT_ARGB8888, 5 /* HAL_PIXEL_FORMAT_BGRA_8888 */, 4, false },
  { DRM_FORMAT_XRGB8888, 5 /* BGRA, alpha ignored via opaque */, 4, true },
  { DRM_FORMAT_RGB565, AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM, 2, true },
};
static const struct Format* find_format(uint32_t drm) {
  for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i)
    if (formats[i].drm == drm) return &formats[i];
  return NULL;
}

bool maton_dmabuf_format_opaque(uint32_t drm_format) {
  const struct Format* f = find_format(drm_format);
  return f && f->opaque;
}

/* Decode the standard mapper metadata wire format (length-prefixed strings
 * and little-endian int64 fields). This compositor targets little-endian
 * Android. Empty plane vectors are a SUCCESS response in minigbm for unknown
 * fourccs, so checking the query return code alone is insufficient. */
struct MetadataReader { const uint8_t* data; size_t left; };
static bool read_i64(struct MetadataReader* r, int64_t* value) {
  if (r->left < sizeof(*value)) return false;
  memcpy(value, r->data, sizeof(*value));
  r->data += sizeof(*value); r->left -= sizeof(*value);
  return true;
}
static bool read_name(struct MetadataReader* r, const char* expected) {
  int64_t len;
  if (!read_i64(r, &len) || len < 0 || (uint64_t)len > r->left) return false;
  if (expected && ((size_t)len != strlen(expected) || memcmp(r->data, expected, (size_t)len))) return false;
  r->data += len; r->left -= len;
  return true;
}

bool maton_dmabuf_layout_safe(AHardwareBuffer* ahb) {
  pthread_once(&resolve_once, resolve);
  if (!ahb || !get_native_handle) return false;
  const struct maton_cros_gralloc_handle* h = (const void*)get_native_handle(ahb);
  if (!h || h->numFds != 1 || h->numInts != (int)((sizeof(*h) - sizeof(native_handle_t)) / sizeof(int)) - 1 ||
      h->magic != MATON_CROS_GRALLOC_MAGIC || h->num_planes != 1 ||
      !h->width || !h->height || !h->strides[0]) return false;
  const struct Format* f = find_format(h->format);
  if (!f) return false;
  uint8_t metadata[4096];
  int32_t size = maton_dmabuf_plane_metadata((const native_handle_t*)h, metadata, sizeof(metadata));
  if (size <= 0 || (size_t)size > sizeof(metadata)) return false;
  struct MetadataReader r = {metadata, (size_t)size};
  int64_t type, planes, components;
  if (!read_name(&r, "android.hardware.graphics.common.StandardMetadataType") ||
      !read_i64(&r, &type) || type != 15 || !read_i64(&r, &planes) || planes != 1 ||
      !read_i64(&r, &components) || components < 1 || components > 4) return false;
  for (int64_t i = 0; i < components; ++i) {
    int64_t component, offset, bits;
    if (!read_name(&r, "android.hardware.graphics.common.PlaneLayoutComponentType") ||
        !read_i64(&r, &component) || !read_i64(&r, &offset) || !read_i64(&r, &bits) ||
        component <= 0 || offset < 0 || bits <= 0 || offset > f->bytes * 8 ||
        bits > f->bytes * 8 - offset) return false;
  }
  int64_t v[8];
  for (size_t i = 0; i < 8; ++i) if (!read_i64(&r, &v[i])) return false;
  return r.left == 0 && v[0] == h->offsets[0] && v[1] == f->bytes * 8 &&
      v[2] == h->strides[0] && v[2] >= (int64_t)h->width * f->bytes &&
      v[3] == h->width && v[4] == h->height && v[5] == h->sizes[0] &&
      (uint64_t)v[5] >= (uint64_t)v[2] * (uint64_t)v[4] && v[6] == 1 && v[7] == 1;
}

static AHardwareBuffer* import(const struct wlr_dmabuf_attributes* d) {
  pthread_once(&resolve_once, resolve);
  const struct Format* f = find_format(d->format);
  if (!create_from_handle || !f || d->n_planes != 1 || d->width <= 0 || d->height <= 0 ||
      (d->modifier != DRM_FORMAT_MOD_LINEAR && d->modifier != DRM_FORMAT_MOD_INVALID) ||
      d->stride[0] < (uint64_t)d->width * f->bytes || d->stride[0] % f->bytes ||
      (uint64_t)d->stride[0] * d->height > UINT32_MAX ||
      (uint64_t)d->offset[0] + (uint64_t)d->stride[0] * d->height > UINT32_MAX) return NULL;
  struct maton_cros_gralloc_handle h;
  memset(&h, 0, sizeof(h));
  h.version = (int32_t)sizeof(native_handle_t);
  h.numFds = 1;
  h.numInts = (int32_t)((sizeof(h) - sizeof(native_handle_t)) / sizeof(int)) - h.numFds;
  for (int i = 0; i < MATON_DRV_MAX_PLANES + 1; ++i) h.fds[i] = -1;
  h.fds[0] = dup(d->fd[0]);
  if (h.fds[0] < 0) return NULL;
  h.strides[0] = d->stride[0];
  h.offsets[0] = d->offset[0];
  h.sizes[0] = d->stride[0] * (uint32_t)d->height;
  h.id = 0;
  h.width = (uint32_t)d->width;
  h.height = (uint32_t)d->height;
  h.format = d->format;
  h.format_modifier = d->modifier;
  h.use_flags = BO_USE_TEXTURE;
  h.magic = MATON_CROS_GRALLOC_MAGIC;
  h.pixel_stride = d->stride[0] / f->bytes;
  h.droid_format = (int32_t)f->ahb;
  h.usage = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER;
  h.num_planes = 1;
  h.total_size = h.offsets[0] + h.sizes[0];
  AHardwareBuffer_Desc desc = {
    .width = h.width, .height = h.height, .layers = 1, .format = f->ahb,
    .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY,
    .stride = h.pixel_stride,
  };
  AHardwareBuffer* ahb = NULL;
  int rc = create_from_handle(&desc, (const native_handle_t*)&h, CREATE_FROM_HANDLE_METHOD_CLONE, &ahb);
  close(h.fds[0]);
  if (rc != 0) {
    __android_log_print(ANDROID_LOG_WARN, "MatonCompositor",
        "dma-buf import failed (rc=%d, %dx%d format 0x%x modifier 0x%llx stride %u)", rc,
        d->width, d->height, d->format, (unsigned long long)d->modifier, d->stride[0]);
    return NULL;
  }
  if (!maton_dmabuf_layout_safe(ahb)) {
    __android_log_print(ANDROID_LOG_WARN, "MatonCompositor",
        "dma-buf rejected: mapper has no complete plane layout for format 0x%x", d->format);
    AHardwareBuffer_release(ahb);
    return NULL;
  }
  return ahb;
}

static bool supported[sizeof(formats) / sizeof(formats[0])];
static pthread_once_t probe_once = PTHREAD_ONCE_INIT;
static void probe_formats(void) {
  pthread_once(&resolve_once, resolve);
  if (!get_native_handle) return;
  for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
    AHardwareBuffer_Desc desc = {
      .width = 64, .height = 64, .layers = 1, .format = formats[i].ahb,
      .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY |
               AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
    };
    AHardwareBuffer* allocated = NULL;
    if (!AHardwareBuffer_isSupported(&desc) || AHardwareBuffer_allocate(&desc, &allocated)) continue;
    const struct maton_cros_gralloc_handle* h = (const void*)get_native_handle(allocated);
    if (h && h->numFds >= 1 && h->numInts >= (int)((sizeof(*h) - sizeof(native_handle_t)) / sizeof(int)) - h->numFds &&
        h->magic == MATON_CROS_GRALLOC_MAGIC && h->num_planes == 1 && h->format_modifier == DRM_FORMAT_MOD_LINEAR) {
      /* Allocate in the Android format, then import under the exact client
       * fourcc. In particular XRGB and ARGB share an AHB format but need
       * different mapper plane-layout descriptions. Probe both offered layouts. */
      struct wlr_dmabuf_attributes d = {
        .width = 64, .height = 64, .format = formats[i].drm,
        .modifier = DRM_FORMAT_MOD_LINEAR, .n_planes = 1,
      };
      d.fd[0] = h->fds[0]; d.stride[0] = h->strides[0]; d.offset[0] = h->offsets[0];
      AHardwareBuffer* linear = import(&d);
      bool linear_ok = linear != NULL;
      if (linear) AHardwareBuffer_release(linear);
      d.modifier = DRM_FORMAT_MOD_INVALID;
      AHardwareBuffer* implicit = import(&d);
      supported[i] = linear_ok && implicit;
      if (implicit) AHardwareBuffer_release(implicit);
    }
    AHardwareBuffer_release(allocated);
    __android_log_print(ANDROID_LOG_INFO, "MatonCompositor", "dma-buf format 0x%x: %s",
        formats[i].drm, supported[i] ? "mapper layout verified" : "disabled");
  }
}
bool maton_dmabuf_format_supported(uint32_t drm_format) {
  pthread_once(&probe_once, probe_formats);
  for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i)
    if (formats[i].drm == drm_format) return supported[i];
  return false;
}

bool maton_dmabuf_importable(const struct wlr_dmabuf_attributes* attrs) {
  AHardwareBuffer* ahb = attrs && maton_dmabuf_format_supported(attrs->format) ? import(attrs) : NULL;
  if (!ahb) return false;
  AHardwareBuffer_release(ahb);
  return true;
}

struct Imported { struct wlr_addon addon; AHardwareBuffer* ahb; };
static void imported_destroy(struct wlr_addon* addon) {
  struct Imported* imp = (struct Imported*)addon;
  if (imp->ahb) AHardwareBuffer_release(imp->ahb);
  wlr_addon_finish(addon);
  free(imp);
}
static const struct wlr_addon_interface imported_impl = {
  .name = "maton_dmabuf_ahb",
  .destroy = imported_destroy,
};

AHardwareBuffer* maton_dmabuf_ahb(struct wlr_buffer* buffer) {
  if (!buffer) return NULL;
  struct wlr_addon* found = wlr_addon_find(&buffer->addons, NULL, &imported_impl);
  if (found) return ((struct Imported*)found)->ahb;
  struct wlr_dmabuf_attributes attrs;
  if (!wlr_buffer_get_dmabuf(buffer, &attrs)) return NULL;
  struct Imported* imp = calloc(1, sizeof(*imp));
  if (!imp) return NULL;
  /* A failed import is cached too (ahb NULL), so it is not retried per frame. */
  imp->ahb = maton_dmabuf_format_supported(attrs.format) ? import(&attrs) : NULL;
  wlr_addon_init(&imp->addon, &buffer->addons, NULL, &imported_impl);
  return imp->ahb;
}
