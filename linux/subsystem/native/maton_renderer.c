#include "maton_renderer.h"

#include <stdlib.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/interface.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>

struct MatonRenderer {
  struct wlr_renderer base;
  struct wlr_renderer* pixman;
  const struct wlr_drm_format_set* dmabuf_formats;
};
static struct MatonRenderer* from_base(struct wlr_renderer* r) { return (struct MatonRenderer*)r; }

/* Like wlroots' GL textures, a stub keeps its dma-buf locked: wl_buffer.release
 * then waits until the texture (and the client buffer holding it) is gone,
 * which the presenter delays until SurfaceFlinger releases the buffer. */
struct StubTexture { struct wlr_texture base; struct wlr_buffer* source; };
static void stub_destroy(struct wlr_texture* texture) {
  struct StubTexture* stub = (struct StubTexture*)texture;
  wlr_buffer_unlock(stub->source);
  free(stub);
}
static const struct wlr_texture_impl stub_texture_impl = { .destroy = stub_destroy };
bool maton_texture_is_stub(const struct wlr_texture* texture) {
  return texture && texture->impl == &stub_texture_impl;
}

static const struct wlr_drm_format_set* get_texture_formats(struct wlr_renderer* r, uint32_t caps) {
  struct MatonRenderer* m = from_base(r);
  if (caps & WLR_BUFFER_CAP_DMABUF) return m->dmabuf_formats;
  return wlr_renderer_get_texture_formats(m->pixman, caps);
}
static const struct wlr_drm_format_set* get_render_formats(struct wlr_renderer* r) {
  struct wlr_renderer* pixman = from_base(r)->pixman;
  return pixman->WLR_PRIVATE.impl->get_render_formats(pixman);
}
static void destroy(struct wlr_renderer* r) {
  struct MatonRenderer* m = from_base(r);
  wlr_renderer_destroy(m->pixman);
  free(m);
}
static struct wlr_texture* texture_from_buffer(struct wlr_renderer* r, struct wlr_buffer* buffer) {
  struct MatonRenderer* m = from_base(r);
  void* data; uint32_t format; size_t stride;
  if (wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
    wlr_buffer_end_data_ptr_access(buffer);
    return wlr_texture_from_buffer(m->pixman, buffer);
  }
  struct wlr_dmabuf_attributes attrs;
  if (!wlr_buffer_get_dmabuf(buffer, &attrs)) return wlr_texture_from_buffer(m->pixman, buffer);
  struct StubTexture* stub = calloc(1, sizeof(*stub));
  if (!stub) return NULL;
  wlr_texture_init(&stub->base, r, &stub_texture_impl, (uint32_t)buffer->width, (uint32_t)buffer->height);
  stub->source = wlr_buffer_lock(buffer);
  return &stub->base;
}
static struct wlr_render_pass* begin_buffer_pass(struct wlr_renderer* r, struct wlr_buffer* buffer,
                                                 const struct wlr_buffer_pass_options* options) {
  return wlr_renderer_begin_buffer_pass(from_base(r)->pixman, buffer, options);
}
static int get_drm_fd(struct wlr_renderer* r) { (void)r; return -1; }

static const struct wlr_renderer_impl renderer_impl = {
  .get_texture_formats = get_texture_formats,
  .get_render_formats = get_render_formats,
  .destroy = destroy,
  .get_drm_fd = get_drm_fd,
  .texture_from_buffer = texture_from_buffer,
  .begin_buffer_pass = begin_buffer_pass,
};

struct wlr_renderer* maton_renderer_create(struct wlr_renderer* pixman,
                                           const struct wlr_drm_format_set* dmabuf_formats) {
  if (!pixman) return NULL;
  struct MatonRenderer* m = calloc(1, sizeof(*m));
  if (!m) return NULL;
  m->pixman = pixman;
  m->dmabuf_formats = dmabuf_formats;
  wlr_renderer_init(&m->base, &renderer_impl, pixman->render_buffer_caps);
  return &m->base;
}
