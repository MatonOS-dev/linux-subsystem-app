#pragma once
#include <stdbool.h>

struct wlr_renderer;
struct wlr_drm_format_set;
struct wlr_texture;

/* Wraps the pixman renderer so client dma-bufs can be committed: their
 * textures are size-only stubs, because windows are presented by handing
 * client buffers to SurfaceFlinger (presenter.c), never by drawing them.
 * dmabuf_formats (borrowed, must outlive the renderer) are reported as the
 * dma-buf texture formats. */
struct wlr_renderer* maton_renderer_create(struct wlr_renderer* pixman,
                                           const struct wlr_drm_format_set* dmabuf_formats);
/* True for stub textures, which must never be drawn. */
bool maton_texture_is_stub(const struct wlr_texture* texture);
