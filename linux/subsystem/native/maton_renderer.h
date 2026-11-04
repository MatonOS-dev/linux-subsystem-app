#pragma once
#include <stdbool.h>

struct wlr_renderer;
struct wlr_texture;

/* Wraps the pixman renderer so client AHB buffers can be committed: their
 * textures are size-only stubs, because windows are presented by handing
 * them to SurfaceFlinger (presenter.c), never by drawing them. */
struct wlr_renderer* maton_renderer_create(struct wlr_renderer* pixman);
/* True for stub textures, which must never be drawn. */
bool maton_texture_is_stub(const struct wlr_texture* texture);
