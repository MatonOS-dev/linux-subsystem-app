/* On-device check for minigbm's external dma-buf import (handles with id 0).
 *
 * Wraps duplicates of gralloc buffers' dma-buf fds in cros_gralloc handles
 * with id 0 and no reserved region (as the compositor will for client
 * dma-bufs) and imports them with AHardwareBuffer_createFromHandle. Checks:
 *  - pixels of an import match its source buffer;
 *  - two different dma-bufs imported at once stay distinct (stock minigbm
 *    tracks buffers by id, so both id-0 imports alias the first);
 *  - a second import of the same dma-buf works;
 *  - metadata (dataspace) can be set and read back without a reserved region.
 * Build with the NDK (API 35) and run on the device as any user. */
#include <android/hardware_buffer.h>
#include <cutils/native_handle.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DRV_MAX_PLANES 4
struct handle {
  int32_t version, numFds, numInts;
  int32_t fds[DRV_MAX_PLANES + 1];
  uint32_t strides[DRV_MAX_PLANES];
  uint32_t offsets[DRV_MAX_PLANES];
  uint32_t sizes[DRV_MAX_PLANES];
  uint32_t id, width, height, format, tiling;
  uint64_t format_modifier, use_flags;
  uint32_t magic, pixel_stride;
  int32_t droid_format;
  int64_t usage;
  uint32_t num_planes;
  uint64_t reserved_region_size, total_size;
} __attribute__((packed));

/* vndk/hardware_buffer.h */
#define CREATE_FROM_HANDLE_METHOD_CLONE 3
#define DATASPACE_SRGB 142671872 /* ADATASPACE_SRGB */
typedef const native_handle_t* (*GetNativeHandle)(const AHardwareBuffer*);
typedef int (*CreateFromHandle)(const AHardwareBuffer_Desc*, const native_handle_t*, int32_t, AHardwareBuffer**);
typedef int (*SetDataSpace)(AHardwareBuffer*, int32_t);
typedef int32_t (*GetDataSpace)(const AHardwareBuffer*);
static GetNativeHandle get_handle;
static CreateFromHandle create;
static SetDataSpace set_dataspace;
static GetDataSpace get_dataspace;

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static const AHardwareBuffer_Desc k_desc = {
  .width = 256, .height = 128, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
  .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
           AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
};

static uint32_t pixel(uint32_t seed, uint32_t x, uint32_t y) { return (seed << 24) | (y << 12) | x; }

/* Allocates a buffer filled from seed; *desc gets its actual description. */
static AHardwareBuffer* allocate_filled(uint32_t seed, AHardwareBuffer_Desc* desc) {
  AHardwareBuffer* buffer = NULL;
  if (AHardwareBuffer_allocate(&k_desc, &buffer)) return NULL;
  memset(desc, 0, sizeof(*desc));
  AHardwareBuffer_describe(buffer, desc);
  desc->rfu0 = 0; desc->rfu1 = 0;
  uint32_t* pixels = NULL;
  if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, (void**)&pixels) || !pixels) {
    AHardwareBuffer_release(buffer);
    return NULL;
  }
  for (uint32_t y = 0; y < desc->height; ++y)
    for (uint32_t x = 0; x < desc->width; ++x) pixels[y * desc->stride + x] = pixel(seed, x, y);
  AHardwareBuffer_unlock(buffer, NULL);
  return buffer;
}

/* Wraps duplicates of the buffer's plane fds in an id-0 handle, imports it. */
static AHardwareBuffer* import_external(AHardwareBuffer* source, const AHardwareBuffer_Desc* desc, int* rc) {
  const struct handle* original = (const struct handle*)get_handle(source);
  *rc = -1;
  if (!original || original->magic != 0xABCDDCBA) return NULL;
  struct handle h;
  memcpy(&h, original, sizeof(h));
  h.version = (int32_t)sizeof(native_handle_t);
  for (uint32_t i = 0; i < DRV_MAX_PLANES + 1; ++i) h.fds[i] = -1;
  for (uint32_t i = 0; i < original->num_planes; ++i) h.fds[i] = dup(original->fds[i]);
  h.numFds = (int32_t)original->num_planes;
  h.numInts = (int32_t)((sizeof(h) - sizeof(native_handle_t)) / sizeof(int)) - h.numFds;
  h.id = 0;
  h.reserved_region_size = 0;
  AHardwareBuffer* imported = NULL;
  *rc = create(desc, (const native_handle_t*)&h, CREATE_FROM_HANDLE_METHOD_CLONE, &imported);
  for (uint32_t i = 0; i < original->num_planes; ++i) close(h.fds[i]);
  return *rc == 0 ? imported : NULL;
}

static int pixels_match(AHardwareBuffer* buffer, const AHardwareBuffer_Desc* desc, uint32_t seed) {
  uint32_t* seen = NULL;
  if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, (void**)&seen) || !seen) return 0;
  int ok = 1;
  for (uint32_t y = 0; y < desc->height && ok; ++y)
    for (uint32_t x = 0; x < desc->width; ++x)
      if (seen[y * desc->stride + x] != pixel(seed, x, y)) { ok = 0; break; }
  AHardwareBuffer_unlock(buffer, NULL);
  return ok;
}

int main(void) {
  void* lib = dlopen("libnativewindow.so", RTLD_NOW);
  get_handle = lib ? (GetNativeHandle)dlsym(lib, "AHardwareBuffer_getNativeHandle") : NULL;
  create = lib ? (CreateFromHandle)dlsym(lib, "AHardwareBuffer_createFromHandle") : NULL;
  set_dataspace = lib ? (SetDataSpace)dlsym(lib, "AHardwareBuffer_setDataSpace") : NULL;
  get_dataspace = lib ? (GetDataSpace)dlsym(lib, "AHardwareBuffer_getDataSpace") : NULL;
  if (!get_handle || !create || !set_dataspace || !get_dataspace) {
    printf("FAIL: libnativewindow handle API missing\n");
    return 1;
  }

  AHardwareBuffer_Desc desc_a, desc_b;
  AHardwareBuffer* a = allocate_filled(0x11, &desc_a);
  AHardwareBuffer* b = allocate_filled(0x22, &desc_b);
  if (!a || !b) { printf("FAIL: allocate\n"); return 1; }

  int rc;
  AHardwareBuffer* import_a = import_external(a, &desc_a, &rc);
  CHECK(import_a, "import A (rc=%d)", rc);
  AHardwareBuffer* import_b = import_external(b, &desc_b, &rc);
  CHECK(import_b, "import B while A is imported (rc=%d)", rc);
  AHardwareBuffer* import_a2 = import_external(a, &desc_a, &rc);
  CHECK(import_a2, "second import of A (rc=%d)", rc);

  if (import_a) CHECK(pixels_match(import_a, &desc_a, 0x11), "import A shows the wrong pixels");
  if (import_b) CHECK(pixels_match(import_b, &desc_b, 0x22), "import B shows the wrong pixels (aliased?)");
  if (import_a2) CHECK(pixels_match(import_a2, &desc_a, 0x11), "second import of A shows the wrong pixels");

  if (import_a) {
    int set = set_dataspace(import_a, DATASPACE_SRGB);
    CHECK(set == 0, "setDataSpace on an import (rc=%d)", set);
    int32_t got = get_dataspace(import_a);
    CHECK(got == DATASPACE_SRGB, "getDataSpace on an import returned %d", got);
  }

  if (import_a2) AHardwareBuffer_release(import_a2);
  if (import_b) AHardwareBuffer_release(import_b);
  if (import_a) AHardwareBuffer_release(import_a);
  AHardwareBuffer_release(b);
  AHardwareBuffer_release(a);
  if (failures) printf("%d FAILED\n", failures);
  else printf("PASS\n");
  return failures ? 1 : 0;
}
