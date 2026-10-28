#include "presenter.h"

#include "android_buffer.h"
#include "android_output.h"
#include "dmabuf_import.h"
#include "ahb_buffer.h"
#include "maton_renderer.h"
#include "presenter_state.h"
#include "presenter_identity.h"

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

struct Layer {
  struct MatonPresenter* presenter;
  struct wlr_scene_buffer* node;
  struct wlr_surface* surface;
  ASurfaceControl* control;
  struct wl_listener node_destroy;
  struct wl_listener surface_destroy;
  struct Layer* next;
  /* What SurfaceFlinger currently shows. */
  struct wlr_buffer* shown;       /* client buffer (dma-buf) or pool buffer (shm copy) */
  struct wlr_buffer* shown_source; /* client buffer the shm copy was made from (compared only) */
  uint32_t shown_seq;             /* surface commit the shm copy was made from */
  bool visible, seen;
  /* Copies of shared-memory content; each is locked while SurfaceFlinger holds it. */
  struct PoolBuffer* pool;
  struct MatonPresenterLayerState state;
};

struct PoolBuffer {
  struct wlr_buffer* buffer;
  struct PoolBuffer* next;
};

struct MatonPresenter {
  ASurfaceControl* parent;
  ASurfaceControl* background;
  ASurfaceTransaction* transaction;
  struct MatonPendingBufferRelease* pending_release;
  struct wlr_allocator* allocator;
  struct MatonEglUploader* uploader;
  struct Layer* layers;
  int background_width, background_height;
  bool background_state_valid, background_visibility_valid, background_visible;
  /* Layers whose scene node is gone; their controls leave the screen in the next transaction. */
  struct Layer* dead;
};

static void layer_free(struct Layer* l) {
  while (l->pool) {
    struct PoolBuffer* entry = l->pool;
    l->pool = entry->next;
    if (entry->buffer) wlr_buffer_drop(entry->buffer);
    free(entry);
  }
  if (l->control) ASurfaceControl_release(l->control);
  free(l);
}

static void remove_listener(struct wl_listener* listener) {
  if (listener->link.next && listener->link.next != &listener->link) {
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
  }
}

/* A scene node can be destroyed without scheduling another output frame.
 * Detach its Android layer here so SurfaceFlinger does not retain a Binder
 * proxy for a layer which is no longer live. Surface-backed scene nodes may
 * be rebuilt while their wl_surface stays alive; those keep their layer and
 * can reuse it when the replacement node appears. */
static void retire_layer(struct MatonPresenter* p, struct Layer* l) {
  struct Layer** link = &p->layers;
  while (*link && *link != l) link = &(*link)->next;
  if (!*link) return;
  *link = l->next;
  maton_transaction_track_replaced(l->control, l->shown, &p->pending_release);
  ASurfaceTransaction_reparent(p->transaction, l->control, NULL);
  maton_transaction_apply(p->transaction, &p->pending_release);
  remove_listener(&l->node_destroy);
  remove_listener(&l->surface_destroy);
  l->node = NULL;
  l->next = NULL;
  layer_free(l);
}

static void on_node_destroy(struct wl_listener* listener, void* data) {
  (void)data;
  struct Layer* l = (struct Layer*)((char*)listener - offsetof(struct Layer, node_destroy));
  remove_listener(&l->node_destroy);
  l->node = NULL;
  if (!l->surface) retire_layer(l->presenter, l);
}

static void on_surface_destroy(struct wl_listener* listener, void* data) {
  (void)data;
  struct Layer* l = (struct Layer*)((char*)listener - offsetof(struct Layer, surface_destroy));
  remove_listener(&l->surface_destroy);
  l->surface = NULL;
  retire_layer(l->presenter, l);
}

static void bind_layer_node(struct Layer* l, struct wlr_scene_buffer* node,
                            struct wlr_surface* surface) {
  if (l->node) remove_listener(&l->node_destroy);
  if (l->surface != surface) {
    remove_listener(&l->surface_destroy);
    l->surface = surface;
    if (surface) {
      l->surface_destroy.notify = on_surface_destroy;
      wl_signal_add(&surface->events.destroy, &l->surface_destroy);
    }
  }
  l->node = node;
  l->node_destroy.notify = on_node_destroy;
  wl_signal_add(&node->node.events.destroy, &l->node_destroy);
}

static struct Layer* find_layer(struct MatonPresenter* p, struct wlr_scene_buffer* node,
                                struct wlr_surface* surface) {
  for (struct Layer* l = p->layers; l; l = l->next) {
    if (!maton_presenter_identity_matches(l->surface, l->node, surface, node)) continue;
    if (l->node != node) {
      bind_layer_node(l, node, surface);
    }
    return l;
  }

  /* Some scene clients replace their buffer nodes as they render while the
   * number of simultaneously visible nodes stays fixed. Keep one native
   * SurfaceControl per visible slot and recycle an unused slot before making
   * another Binder-backed SurfaceControl. Exact surface/node matches above
   * still win, so stable Wayland surfaces retain their own layer and SHM pool. */
  for (struct Layer* l = p->layers; l; l = l->next) {
    /* A still-visible node may occur later in this frame's traversal. Do not
     * steal its control before its exact-identity lookup gets a chance. */
    if (l->seen || (l->visible && l->node)) continue;
    bind_layer_node(l, node, surface);
    l->state.valid = false;
    return l;
  }

  struct Layer* l = calloc(1, sizeof(*l));
  if (!l) return NULL;
  l->control = ASurfaceControl_create(p->parent, "maton-surface");
  if (!l->control) { free(l); return NULL; }
  l->presenter = p;
  bind_layer_node(l, node, surface);
  l->next = p->layers;
  p->layers = l;
  return l;
}

/* A pool buffer SurfaceFlinger is not holding. The pool owns its buffers
 * without a lock (wlroots' "dropped" flag), so a free buffer has no locks;
 * maton_transaction_set_buffer locks one until SurfaceFlinger releases it. */
static struct wlr_buffer* free_pool_buffer(struct MatonPresenter* p, struct Layer* l, int width, int height) {
  /* Search the whole pool before asking the allocator. A failed allocation in
   * an early hole must not hide a free buffer later in the list. */
  struct PoolBuffer* reusable = NULL;
  for (struct PoolBuffer* entry = l->pool; entry; entry = entry->next) {
    struct wlr_buffer* b = entry->buffer;
    if (!b || b->n_locks != 0) continue;
    if (b->width == width && b->height == height) return b;
    if (!reusable) reusable = entry;
  }
  struct wlr_drm_format format = {.format = DRM_FORMAT_ARGB8888, .len = 1, .capacity = 1};
  uint64_t linear = DRM_FORMAT_MOD_LINEAR;
  format.modifiers = &linear;
  struct wlr_buffer* replacement = wlr_allocator_create_buffer(p->allocator, width, height, &format);
  if (!replacement) return NULL;
  if (reusable) {
    wlr_buffer_drop(reusable->buffer);
    reusable->buffer = replacement;
    return replacement;
  }
  struct PoolBuffer* entry = calloc(1, sizeof(*entry));
  if (!entry) {
    wlr_buffer_drop(replacement);
    return NULL;
  }
  entry->buffer = replacement;
  entry->next = l->pool;
  l->pool = entry;
  return replacement;
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

struct Walk { struct MatonPresenter* p; ASurfaceTransaction* t; int z; bool pending, changed; struct MatonFit fit; };

static void present_node(struct wlr_scene_buffer* node, int sx, int sy, void* data) {
  struct Walk* walk = data;
  struct MatonPresenter* p = walk->p;
  struct wlr_buffer* buffer = node->buffer;
  if (!buffer) return;
  struct wlr_scene_surface* scene_surface = wlr_scene_surface_try_from_buffer(node);
  struct wlr_surface* surface = scene_surface ? scene_surface->surface : NULL;
  struct Layer* l = find_layer(p, node, surface);
  if (!l) return;
  l->seen = true;
  ASurfaceTransaction* t = walk->t;

  uint32_t seq = scene_surface ? scene_surface->surface->current.seq : 0;
  AHardwareBuffer* ahb = maton_ahb_buffer_get(buffer);
  bool direct_ahb = ahb != NULL;
  if (!ahb) ahb = maton_dmabuf_ahb(buffer);
  /* Defence in depth: never send a dma-buf whose actual mapper metadata
   * cannot describe its planes, even if it passed protocol-time validation. */
  if (ahb && !direct_ahb && !maton_dmabuf_layout_safe(ahb)) ahb = NULL;
  bool opaque = false;
  if (ahb) {
    struct wlr_dmabuf_attributes attrs;
    if (wlr_buffer_get_dmabuf(buffer, &attrs)) opaque = maton_dmabuf_format_opaque(attrs.format);
    if (buffer != l->shown || seq != l->shown_seq) {
      int acquire_fence=scene_surface?maton_sync_take_acquire(scene_surface->surface):-1;
      maton_transaction_set_buffer(t, l->control, ahb, buffer, l->shown,
                                   &p->pending_release, acquire_fence);
      walk->changed = true;
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
    maton_transaction_set_buffer(t, l->control, maton_ahb_from_wlr_buffer(copy), copy, l->shown,
                                 &p->pending_release, -1);
    walk->changed = true;
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
  int z = walk->z++;
  enum ASurfaceTransactionTransparency transparency = opaque && node->opacity >= 1.0f
      ? ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE : ASURFACE_TRANSACTION_TRANSPARENCY_TRANSLUCENT;
  int src_values[] = {src.left, src.top, src.right, src.bottom};
  int dst_values[] = {dst.left, dst.top, dst.right, dst.bottom};
  unsigned state_changes = maton_presenter_layer_state_update(
      &l->state, src_values, dst_values, z, node->opacity, transparency);
  if (state_changes & MATON_PRESENTER_GEOMETRY_CHANGED) {
    ASurfaceTransaction_setGeometry(t, l->control, &src, &dst, ANATIVEWINDOW_TRANSFORM_IDENTITY);
    walk->changed = true;
  }
  if (state_changes & MATON_PRESENTER_Z_CHANGED) {
    ASurfaceTransaction_setZOrder(t, l->control, z); walk->changed = true;
  }
  if (state_changes & MATON_PRESENTER_OPACITY_CHANGED) {
    ASurfaceTransaction_setBufferAlpha(t, l->control, node->opacity);
    walk->changed = true;
  }
  if (state_changes & MATON_PRESENTER_TRANSPARENCY_CHANGED) {
    ASurfaceTransaction_setBufferTransparency(t, l->control, transparency);
    walk->changed = true;
  }
  if (!l->visible) {
    ASurfaceTransaction_setVisibility(t, l->control, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    l->visible = true;
    walk->changed = true;
  }
}

struct MatonPresenter* maton_presenter_create(ASurfaceControl* parent, struct wlr_allocator* allocator,
                                              struct MatonEglUploader* uploader) {
  if (!parent || !allocator) return NULL;
  struct MatonPresenter* p = calloc(1, sizeof(*p));
  if (!p) return NULL;
  p->transaction = ASurfaceTransaction_create();
  p->background=ASurfaceControl_create(parent,"maton-background");
  if(!p->background || !p->transaction){
    if(p->background)ASurfaceControl_release(p->background);
    if(p->transaction)ASurfaceTransaction_delete(p->transaction);
    free(p);return NULL;
  }
  p->parent = parent;
  p->allocator = allocator;
  p->uploader = uploader;
  return p;
}

bool maton_presenter_present(struct MatonPresenter* p, struct wlr_scene* scene, struct MatonFit fit, int width, int height) {
  if (!p || !scene) return false;
  ASurfaceTransaction* t = p->transaction;
  if (!t) return true;
  for (struct Layer* l = p->layers; l; l = l->next) l->seen = false;
  ARect bounds={0,0,width,height};
  bool changed = false;
  if (!p->background_state_valid || p->background_width != width || p->background_height != height) {
    ASurfaceTransaction_setColor(t,p->background,0,0,0,1,ADATASPACE_SRGB);
    ASurfaceTransaction_setCrop(t,p->background,&bounds);
    ASurfaceTransaction_setZOrder(t,p->background,-1);
    p->background_width = width; p->background_height = height;
    p->background_state_valid = true; changed = true;
  }
  bool show_background = !(fit.scale==1&&fit.x==0&&fit.y==0);
  if (!p->background_visibility_valid || p->background_visible != show_background) {
    ASurfaceTransaction_setVisibility(t,p->background,show_background?ASURFACE_TRANSACTION_VISIBILITY_SHOW:ASURFACE_TRANSACTION_VISIBILITY_HIDE);
    p->background_visible = show_background; p->background_visibility_valid = true; changed = true;
  }
  struct Walk walk = {p, t, 0, false, changed, fit};
  wlr_scene_node_for_each_buffer(&scene->tree.node, present_node, &walk);
  /* Hide layers not drawn this frame; detach those whose node is gone. */
  struct Layer** link = &p->layers;
  while (*link) {
    struct Layer* l = *link;
    if (!l->node) {
      /* A wlr_scene_buffer can disappear while its wl_surface is still
       * alive. Hide its layer and leave the SurfaceControl reusable for that
       * surface's next scene node (or for another unused slot). */
      if (l->surface) {
        if (l->visible) {
          ASurfaceTransaction_setVisibility(t, l->control, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
          l->visible = false;
          walk.changed = true;
        }
        link = &l->next;
        continue;
      }
      maton_transaction_track_replaced(l->control, l->shown, &p->pending_release);
      ASurfaceTransaction_reparent(t, l->control, NULL);
      walk.changed = true;
      *link = l->next;
      l->next = p->dead;
      p->dead = l;
      continue;
    }
    if (!l->seen && l->visible) {
      ASurfaceTransaction_setVisibility(t, l->control, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
      l->visible = false;
      walk.changed = true;
    }
    link = &l->next;
  }
  if (walk.changed) maton_transaction_apply(t, &p->pending_release);
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
  struct MatonPendingBufferRelease* pending_release = NULL;
  for (struct Layer* l = p->layers; l; l = l->next) {
    remove_listener(&l->node_destroy);
    remove_listener(&l->surface_destroy);
    if (t) {
      maton_transaction_track_replaced(l->control, l->shown, &pending_release);
      ASurfaceTransaction_reparent(t, l->control, NULL);
    }
  }
  if (t) { ASurfaceTransaction_reparent(t,p->background,NULL); maton_transaction_apply(t,&pending_release); ASurfaceTransaction_delete(t); }
  ASurfaceControl_release(p->background);
  ASurfaceTransaction_delete(p->transaction);
  while (p->layers) { struct Layer* l = p->layers; p->layers = l->next; layer_free(l); }
  free(p);
}
