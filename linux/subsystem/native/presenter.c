#include "presenter.h"

#include "android_buffer.h"
#include "android_output.h"
#include "dmabuf_import.h"
#include "maton_renderer.h"

#include <android/data_space.h>
#include <android/log.h>
#include <drm/drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>

#define SHM_POOL 4  /* SurfaceFlinger can hold three at once (shown, queued, releasing) */

struct Layer {
  struct wlr_scene_buffer* node;
  ASurfaceControl* control;
  struct wl_listener node_destroy;
  struct Layer* next;
  /* What SurfaceFlinger currently shows. */
  struct wlr_buffer* shown;       /* client buffer (dma-buf) or pool buffer (shm copy) */
  struct wlr_buffer* shown_source; /* client buffer the shm copy was made from (compared only) */
  uint32_t shown_seq;             /* surface commit the shm copy was made from */
  bool visible, seen;
  /* Copies of shared-memory content; each is locked while SurfaceFlinger holds it. */
  struct wlr_buffer* pool[SHM_POOL];
};

struct MatonPresenter {
  ASurfaceControl* parent;
  ASurfaceControl* background;
  struct wlr_allocator* allocator;
  struct MatonEglUploader* uploader;
  struct Layer* layers;
  /* Layers whose scene node is gone; their controls leave the screen in the next transaction. */
  struct Layer* dead;
};

static void layer_free(struct Layer* l) {
  for (int i = 0; i < SHM_POOL; ++i) if (l->pool[i]) wlr_buffer_drop(l->pool[i]);
  if (l->control) ASurfaceControl_release(l->control);
  free(l);
}

static void on_node_destroy(struct wl_listener* listener, void* data) {
  (void)data;
  struct Layer* l = (struct Layer*)((char*)listener - offsetof(struct Layer, node_destroy));
  wl_list_remove(&l->node_destroy.link);
  l->node = NULL;
}

static struct Layer* find_layer(struct MatonPresenter* p, struct wlr_scene_buffer* node) {
  for (struct Layer* l = p->layers; l; l = l->next) if (l->node == node) return l;
  struct Layer* l = calloc(1, sizeof(*l));
  if (!l) return NULL;
  l->control = ASurfaceControl_create(p->parent, "maton-surface");
  if (!l->control) { free(l); return NULL; }
  l->node = node;
  l->node_destroy.notify = on_node_destroy;
  wl_signal_add(&node->node.events.destroy, &l->node_destroy);
  l->next = p->layers;
  p->layers = l;
  return l;
}

/* A pool buffer SurfaceFlinger is not holding. The pool owns its buffers
 * without a lock (wlroots' "dropped" flag), so a free buffer has no locks;
 * maton_transaction_set_buffer locks one until SurfaceFlinger releases it. */
static struct wlr_buffer* free_pool_buffer(struct MatonPresenter* p, struct Layer* l, int width, int height) {
  for (int i = 0; i < SHM_POOL; ++i) {
    struct wlr_buffer* b = l->pool[i];
    if (b && b->n_locks == 0 && (b->width != width || b->height != height)) {
      wlr_buffer_drop(b);
      l->pool[i] = b = NULL;
    }
    if (b && b->n_locks == 0) return b;
    if (!b) {
      struct wlr_drm_format format = {.format = DRM_FORMAT_ARGB8888, .len = 1, .capacity = 1};
      uint64_t linear = DRM_FORMAT_MOD_LINEAR;
      format.modifiers = &linear;
      l->pool[i] = wlr_allocator_create_buffer(p->allocator, width, height, &format);
      return l->pool[i];
    }
  }
  return NULL;
}

/* Copies shared-memory content into a pool buffer. It is read from the
 * client buffer's texture: for shm, wlroots updates that texture in place on
 * later commits while the client buffer's source still names the first
 * wl_buffer (wlr_client_buffer_apply_damage). */
static struct wlr_buffer* copy_shm(struct MatonPresenter* p, struct Layer* l, struct wlr_buffer* source) {
  struct wlr_client_buffer* client = wlr_client_buffer_get(source);
  struct wlr_texture* texture = client ? client->texture : NULL;
  const char* why = NULL;
  struct wlr_buffer* target = NULL;
  if (!client) why = "not a client buffer";
  else if (!texture) why = "no texture";
  else if (maton_texture_is_stub(texture)) why = "stub texture";
  else if (!(target = free_pool_buffer(p, l, (int)texture->width, (int)texture->height))) why = "no pool buffer";
  void* dst = NULL; uint32_t dst_format; size_t dst_stride = 0;
  if (!why && !wlr_buffer_begin_data_ptr_access(target, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &dst, &dst_format, &dst_stride))
    why = "pool buffer not writable";
  if (!why) {
    struct wlr_texture_read_pixels_options read = {
      .data = dst, .format = DRM_FORMAT_ARGB8888, .stride = (uint32_t)dst_stride,
    };
    bool ok = wlr_texture_read_pixels(texture, &read);
    wlr_buffer_end_data_ptr_access(target);
    if (!ok) why = "read_pixels failed";
    else if (!maton_ahb_buffer_is_direct(target) && !maton_ahb_buffer_upload_fallback(target, p->uploader))
      why = "upload fallback failed";
  }
  if (why) {
    __android_log_print(ANDROID_LOG_WARN, "MatonPresenter", "shm copy: %s (buffer %dx%d, texture %ux%u)", why,
                        source->width, source->height, texture ? texture->width : 0, texture ? texture->height : 0);
    return NULL;
  }
  return target;
}

struct Walk { struct MatonPresenter* p; ASurfaceTransaction* t; int z; bool pending; struct MatonFit fit; };

static void present_node(struct wlr_scene_buffer* node, int sx, int sy, void* data) {
  struct Walk* walk = data;
  struct MatonPresenter* p = walk->p;
  struct wlr_buffer* buffer = node->buffer;
  if (!buffer) return;
  struct Layer* l = find_layer(p, node);
  if (!l) return;
  l->seen = true;
  ASurfaceTransaction* t = walk->t;

  struct wlr_scene_surface* scene_surface = wlr_scene_surface_try_from_buffer(node);
  uint32_t seq = scene_surface ? scene_surface->surface->current.seq : 0;
  AHardwareBuffer* ahb = maton_dmabuf_ahb(buffer);
  /* Defence in depth: never send a dma-buf whose actual mapper metadata
   * cannot describe its planes, even if it passed protocol-time validation. */
  if (ahb && !maton_dmabuf_layout_safe(ahb)) ahb = NULL;
  bool opaque = false;
  if (ahb) {
    struct wlr_dmabuf_attributes attrs;
    if (wlr_buffer_get_dmabuf(buffer, &attrs)) opaque = maton_dmabuf_format_opaque(attrs.format);
    if (buffer != l->shown || seq != l->shown_seq) {
      int acquire_fence=scene_surface?maton_sync_take_acquire(scene_surface->surface):-1;
      maton_transaction_set_buffer(t, l->control, ahb, buffer, acquire_fence);
      l->shown = buffer;
      l->shown_source = NULL;
      l->shown_seq = seq;
    }
  } else if (buffer != l->shown_source || seq != l->shown_seq || !l->shown) {
    struct wlr_buffer* copy = copy_shm(p, l, buffer);
    if (!copy) {
      /* GPU textures may be stubs with no readback. Hide just this window;
       * the other Linux layers and Android surfaces remain composable. */
      struct wlr_dmabuf_attributes attrs;
      if (wlr_buffer_get_dmabuf(buffer, &attrs)) {
        l->seen = false;
        l->shown_source = NULL;
      } else walk->pending = true;
      return;
    }
    maton_transaction_set_buffer(t, l->control, maton_ahb_from_wlr_buffer(copy), copy, -1);
    l->shown = copy;
    l->shown_source = buffer;
    l->shown_seq = seq;
  }
  opaque = opaque || wlr_buffer_is_opaque(buffer);

  /* Source crop (viewporter) and destination size; buffer coordinates. */
  ARect src = {0, 0, buffer->width, buffer->height};
  if (!wlr_fbox_empty(&node->src_box)) {
    src.left = (int32_t)node->src_box.x;
    src.top = (int32_t)node->src_box.y;
    src.right = (int32_t)(node->src_box.x + node->src_box.width);
    src.bottom = (int32_t)(node->src_box.y + node->src_box.height);
  }
  int width = node->dst_width > 0 ? node->dst_width : src.right - src.left;
  int height = node->dst_height > 0 ? node->dst_height : src.bottom - src.top;
  ARect dst = {maton_fit_edge(walk->fit.x,walk->fit.scale,sx),
               maton_fit_edge(walk->fit.y,walk->fit.scale,sy),
               maton_fit_edge(walk->fit.x,walk->fit.scale,sx+width),
               maton_fit_edge(walk->fit.y,walk->fit.scale,sy+height)};
  ASurfaceTransaction_setGeometry(t, l->control, &src, &dst, ANATIVEWINDOW_TRANSFORM_IDENTITY);
  ASurfaceTransaction_setZOrder(t, l->control, walk->z++);
  ASurfaceTransaction_setBufferAlpha(t, l->control, node->opacity);
  ASurfaceTransaction_setBufferTransparency(t, l->control,
      opaque && node->opacity >= 1.0f ? ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE
                                      : ASURFACE_TRANSACTION_TRANSPARENCY_TRANSLUCENT);
  if (!l->visible) {
    ASurfaceTransaction_setVisibility(t, l->control, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    l->visible = true;
  }
}

struct MatonPresenter* maton_presenter_create(ASurfaceControl* parent, struct wlr_allocator* allocator,
                                              struct MatonEglUploader* uploader) {
  if (!parent || !allocator) return NULL;
  struct MatonPresenter* p = calloc(1, sizeof(*p));
  if (!p) return NULL;
  p->background=ASurfaceControl_create(parent,"maton-background");
  if(!p->background){free(p);return NULL;}
  p->parent = parent;
  p->allocator = allocator;
  p->uploader = uploader;
  return p;
}

bool maton_presenter_present(struct MatonPresenter* p, struct wlr_scene* scene, struct MatonFit fit, int width, int height) {
  if (!p || !scene) return false;
  ASurfaceTransaction* t = ASurfaceTransaction_create();
  if (!t) return true;
  for (struct Layer* l = p->layers; l; l = l->next) l->seen = false;
  ARect bounds={0,0,width,height};
  ASurfaceTransaction_setColor(t,p->background,0,0,0,1,ADATASPACE_SRGB);
  ASurfaceTransaction_setCrop(t,p->background,&bounds);
  ASurfaceTransaction_setZOrder(t,p->background,-1);
  ASurfaceTransaction_setVisibility(t,p->background,
      fit.scale==1&&fit.x==0&&fit.y==0?ASURFACE_TRANSACTION_VISIBILITY_HIDE:ASURFACE_TRANSACTION_VISIBILITY_SHOW);
  struct Walk walk = {p, t, 0, false, fit};
  wlr_scene_node_for_each_buffer(&scene->tree.node, present_node, &walk);
  /* Hide layers not drawn this frame; detach those whose node is gone. */
  struct Layer** link = &p->layers;
  while (*link) {
    struct Layer* l = *link;
    if (!l->node) {
      ASurfaceTransaction_reparent(t, l->control, NULL);
      *link = l->next;
      l->next = p->dead;
      p->dead = l;
      continue;
    }
    if (!l->seen && l->visible) {
      ASurfaceTransaction_setVisibility(t, l->control, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
      l->visible = false;
    }
    link = &l->next;
  }
  ASurfaceTransaction_apply(t);
  ASurfaceTransaction_delete(t);
  /* Detached layers' controls are off screen once the transaction applied. */
  while (p->dead) {
    struct Layer* l = p->dead;
    p->dead = l->next;
    layer_free(l);
  }
  return walk.pending;
}

void maton_presenter_destroy(struct MatonPresenter* p) {
  if (!p) return;
  ASurfaceTransaction* t = ASurfaceTransaction_create();
  for (struct Layer* l = p->layers; l; l = l->next) {
    if (l->node) wl_list_remove(&l->node_destroy.link);
    if (t) ASurfaceTransaction_reparent(t, l->control, NULL);
  }
  if (t) { ASurfaceTransaction_reparent(t,p->background,NULL); ASurfaceTransaction_apply(t); ASurfaceTransaction_delete(t); }
  ASurfaceControl_release(p->background);
  while (p->layers) { struct Layer* l = p->layers; p->layers = l->next; layer_free(l); }
  free(p);
}
