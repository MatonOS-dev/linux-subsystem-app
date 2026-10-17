#include "wsi_wayland.h"
#include "wayland_resolve.h"
#include "runtime.h"

/* libwayland-client ABI layout, stable since Wayland 1.0. */
struct wl_message { const char *name, *signature; const struct wl_interface **types; };
struct wl_interface {
    const char *name;
    int version, method_count;
    const struct wl_message *methods;
    int event_count;
    const struct wl_message *events;
};
typedef struct wl_proxy *(*marshal_fn)(struct wl_proxy *, uint32_t,
        const struct wl_interface *, uint32_t, uint32_t, ...);
typedef uint32_t (*version_fn)(struct wl_proxy *);
typedef int (*listener_fn)(struct wl_proxy *, void (**)(void), void *);
typedef void (*destroy_fn)(struct wl_proxy *);
typedef struct wl_event_queue *(*queue_create_fn)(struct wl_display *);
typedef void (*queue_destroy_fn)(struct wl_event_queue *);
typedef int (*roundtrip_fn)(struct wl_display *, struct wl_event_queue *);
typedef struct wl_proxy *(*get_registry_fn)(struct wl_display *);
typedef struct wl_proxy *(*wrapper_fn)(struct wl_proxy *);
typedef void (*wrapper_destroy_fn)(struct wl_proxy *);
typedef void (*set_queue_fn)(struct wl_proxy *, struct wl_event_queue *);

struct mv_wl_dmabuf {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wl_proxy *registry, *proxy, *wrapper;
    uint32_t global_name;
    int version;
};
struct mv_wl_buffer { struct wl_proxy *proxy; int released; };

static marshal_fn marshal;
static version_fn proxy_version;
static listener_fn add_listener;
static destroy_fn proxy_destroy;
static queue_create_fn queue_create;
static queue_destroy_fn queue_destroy;
static roundtrip_fn roundtrip;
static get_registry_fn get_registry;
static wrapper_fn create_wrapper;
static wrapper_destroy_fn destroy_wrapper;
static set_queue_fn set_queue;
static const struct wl_interface *registry_interface;
static int resolved;

static void *sym(const char *name) { return mv_wayland_symbol(name); }
static int resolve(void) {
    if (resolved) return marshal && proxy_version && add_listener && proxy_destroy &&
        queue_create && queue_destroy && roundtrip && get_registry && create_wrapper &&
        destroy_wrapper && set_queue && registry_interface;
    resolved = 1;
    marshal=(marshal_fn)sym("wl_proxy_marshal_flags");
    proxy_version=(version_fn)sym("wl_proxy_get_version");
    add_listener=(listener_fn)sym("wl_proxy_add_listener");
    proxy_destroy=(destroy_fn)sym("wl_proxy_destroy");
    queue_create=(queue_create_fn)sym("wl_display_create_queue");
    queue_destroy=(queue_destroy_fn)sym("wl_event_queue_destroy");
    roundtrip=(roundtrip_fn)sym("wl_display_roundtrip_queue");
    get_registry=(get_registry_fn)sym("wl_display_get_registry");
    create_wrapper=(wrapper_fn)sym("wl_proxy_create_wrapper");
    destroy_wrapper=(wrapper_destroy_fn)sym("wl_proxy_wrapper_destroy");
    set_queue=(set_queue_fn)sym("wl_proxy_set_queue");
    registry_interface=(const struct wl_interface *)sym("wl_registry_interface");
    return marshal && proxy_version && add_listener && proxy_destroy && queue_create &&
        queue_destroy && roundtrip && get_registry && create_wrapper && destroy_wrapper &&
        set_queue && registry_interface;
}

static const struct wl_interface params_interface, buffer_interface;
static const struct wl_interface *dmabuf_params_types[] = { &params_interface };
static const struct wl_interface *params_create_types[] = { 0, 0, 0, 0, &buffer_interface };
static const struct wl_interface *params_create_immed_types[] = { &buffer_interface, 0, 0, 0, 0 };
static const struct wl_interface *params_created_types[] = { &buffer_interface };
static const struct wl_message dmabuf_methods[] = {
    {"destroy", "1", 0}, {"create_params", "n", dmabuf_params_types},
};
static const struct wl_message dmabuf_events[] = {
    {"format", "u", 0}, {"modifier", "3uuu", 0},
};
static const struct wl_message params_methods[] = {
    {"destroy", "", 0}, {"add", "huuuuu", 0},
    {"create", "iiuun", params_create_types},
    {"create_immed", "niiuu", params_create_immed_types},
};
static const struct wl_message params_events[] = {
    {"created", "n", params_created_types}, {"failed", "", 0},
};
static const struct wl_message buffer_events[] = {{"release", "", 0}};
static const struct wl_interface params_interface = {
    "zwp_linux_buffer_params_v1", 4, 4, params_methods, 2, params_events,
};
static const struct wl_interface buffer_interface = {
    "wl_buffer", 1, 1, 0, 1, buffer_events,
};
static const struct wl_interface dmabuf_interface = {
    "zwp_linux_dmabuf_v1", 4, 2, dmabuf_methods, 2, dmabuf_events,
};
static void registry_global(void *data, struct wl_proxy *registry, uint32_t name,
        const char *interface, uint32_t version) {
    (void)registry;
    struct mv_wl_dmabuf *d=data;
    if (d->global_name || !interface || strcmp(interface,"zwp_linux_dmabuf_v1")) return;
    d->global_name=name; d->version=version>4?4:(int)version;
}
static void registry_remove(void *data, struct wl_proxy *registry, uint32_t name) {
    (void)registry; struct mv_wl_dmabuf *d=data;
    if (d->global_name==name) { d->global_name=0; d->version=0; }
}
static void (*const registry_listener[])(void) = {
    (void (*)(void))registry_global, (void (*)(void))registry_remove,
};
static void buffer_release(void *data, struct wl_proxy *proxy) {
    (void)proxy; ((struct mv_wl_buffer *)data)->released=1;
}
static void (*const buffer_listener[])(void) = {(void (*)(void))buffer_release};

struct mv_wl_dmabuf *mv_wl_dmabuf_create(struct wl_display *display) {
    if (!display || !resolve()) return 0;
    struct mv_wl_dmabuf *d=mv_alloc(sizeof(*d)); if (!d) return 0;
    memset(d,0,sizeof(*d)); d->display=display;
    d->queue=queue_create(display); if (!d->queue) goto fail;
    d->registry=get_registry(display); if (!d->registry) goto fail;
    set_queue(d->registry,d->queue);
    if (add_listener(d->registry,(void (**)(void))registry_listener,d)) goto fail;
    if (roundtrip(display,d->queue)<0 || !d->global_name || d->version<2) goto fail;
    d->proxy=marshal(d->registry,0,&dmabuf_interface,(uint32_t)d->version,0,
            d->global_name,dmabuf_interface.name,(uint32_t)d->version,(void *)0);
    if (!d->proxy) goto fail;
    set_queue(d->proxy,d->queue);
    d->wrapper=create_wrapper(d->proxy); if (!d->wrapper) goto fail;
    set_queue(d->wrapper,d->queue);
    return d;
fail:
    mv_wl_dmabuf_destroy(d); return 0;
}
void mv_wl_dmabuf_destroy(struct mv_wl_dmabuf *d) {
    if (!d) return;
    if (d->wrapper) destroy_wrapper(d->wrapper);
    if (d->proxy) proxy_destroy(d->proxy);
    if (d->registry) proxy_destroy(d->registry);
    if (d->queue) queue_destroy(d->queue);
    mv_free(d);
}
int mv_wl_dmabuf_version(const struct mv_wl_dmabuf *d) { return d?d->version:0; }

struct mv_wl_buffer *mv_wl_dmabuf_create_buffer(struct mv_wl_dmabuf *d,
        uint32_t width,uint32_t height,uint32_t format,const struct mv_wl_plane *planes,uint32_t n) {
    if (!d || !d->proxy || !width || !height || !planes || !n || n>4) return 0;
    struct wl_proxy *params=marshal(d->wrapper,1,&params_interface,
            (uint32_t)d->version,0,(void *)0);
    if (!params) return 0;
    set_queue(params,d->queue);
    for (uint32_t i=0;i<n;i++) {
        if (planes[i].fd<0 || planes[i].index>=4 || !planes[i].stride) {
            proxy_destroy(params); return 0;
        }
        marshal(params,1,0,proxy_version(params),0,planes[i].fd,planes[i].index,
                planes[i].offset,planes[i].stride,(uint32_t)(planes[i].modifier>>32),
                (uint32_t)planes[i].modifier);
    }
    struct wl_proxy *raw=marshal(params,3,&buffer_interface,1,0,(void *)0,
            (int32_t)width,(int32_t)height,format,0);
    proxy_destroy(params);
    if (!raw) return 0;
    set_queue(raw,d->queue);
    struct mv_wl_buffer *b=mv_alloc(sizeof(*b));
    if (!b) { proxy_destroy(raw); return 0; }
    b->proxy=raw; b->released=0;
    if (add_listener(raw,(void (**)(void))buffer_listener,b)) {
        proxy_destroy(raw); mv_free(b); return 0;
    }
    return b;
}
void mv_wl_buffer_destroy(struct mv_wl_buffer *b) {
    if (!b) return;
    if (b->proxy) proxy_destroy(b->proxy);
    mv_free(b);
}
int mv_wl_buffer_released(const struct mv_wl_buffer *b) { return b && b->released; }
void mv_wl_buffer_clear_released(struct mv_wl_buffer *b) { if (b) b->released=0; }
