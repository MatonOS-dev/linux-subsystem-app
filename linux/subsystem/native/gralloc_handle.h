#pragma once
/* C mirror of minigbm's packed cros_gralloc_handle (cros_gralloc_handle.h in
 * external/minigbm, MatonOS fork). The layout must match the device's
 * minigbm exactly; it is shared by every process that imports the handle. */
#include <cutils/native_handle.h>
#include <stdint.h>

#define MATON_CROS_GRALLOC_MAGIC 0xABCDDCBAu
#define MATON_DRV_MAX_PLANES 4

struct maton_cros_gralloc_handle {
  int32_t version, numFds, numInts;
  int32_t fds[MATON_DRV_MAX_PLANES + 1];
  uint32_t strides[MATON_DRV_MAX_PLANES];
  uint32_t offsets[MATON_DRV_MAX_PLANES];
  uint32_t sizes[MATON_DRV_MAX_PLANES];
  uint32_t id, width, height, format, tiling;
  uint64_t format_modifier, use_flags;
  uint32_t magic, pixel_stride;
  int32_t droid_format;
  int64_t usage;
  uint32_t num_planes;
  uint64_t reserved_region_size, total_size;
} __attribute__((packed));
