#pragma once
#include <android/hardware_buffer.h>
#include <stdbool.h>
#include <stdint.h>

struct wlr_buffer;

/* Runtime allocation + external-import + mapper plane-layout probe, cached
 * per format. A known fourcc mapping alone never implies support. */
bool maton_dmabuf_format_supported(uint32_t drm_format);
/* True if the format has no alpha channel (SurfaceFlinger must treat the
 * layer as opaque; the X byte is undefined). */
bool maton_dmabuf_format_opaque(uint32_t drm_format);
/* The AHardwareBuffer wrapping a client's dma-buf, imported on first use and
 * cached for the buffer's lifetime (borrowed). NULL for non-dma-buf buffers
 * and buffers without a complete mapper plane layout. */
AHardwareBuffer* maton_dmabuf_ahb(struct wlr_buffer* buffer);
/* True if gralloc can import and fully describe the dma-buf (tries it); used to refuse
 * linux-dmabuf buffers at creation instead of showing nothing later. */
struct wlr_dmabuf_attributes;
bool maton_dmabuf_importable(const struct wlr_dmabuf_attributes* attrs);

/* Query and validate the actual imported handle before direct presentation. */
bool maton_dmabuf_layout_safe(AHardwareBuffer* ahb);
