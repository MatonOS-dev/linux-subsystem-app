#include "wayland_client.h"
#include "../vulkan-forward/wayland_resolve.h"
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
typedef int (*dispatch_pending_fn)(struct wl_display *, struct wl_event_queue *);
typedef int (*dispatch_queue_fn)(struct wl_display *, struct wl_event_queue *);
typedef struct wl_proxy *(*wrapper_fn)(struct wl_proxy *);
typedef void (*wrapper_destroy_fn)(struct wl_proxy *);
typedef void (*set_queue_fn)(struct wl_proxy *, struct wl_event_queue *);
typedef int (*flush_fn)(struct wl_display *);

struct mv_wl_dmabuf {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wl_proxy *registry, *proxy, *wrapper;
    struct wl_proxy *sync_manager, *ahb_manager;
    uint32_t global_name, sync_global_name, ahb_global_name;
    int version, sync_version, ahb_version;
};
struct mv_wl_buffer { struct wl_proxy *proxy; int released; };
struct mv_wl_sync_surface { struct wl_proxy *proxy; struct wl_display *display; struct wl_event_queue *queue; struct wl_proxy *frame; int frame_done; };

static marshal_fn marshal;
static version_fn proxy_version;
static listener_fn add_listener;
static destroy_fn proxy_destroy;
static queue_create_fn queue_create;
static queue_destroy_fn queue_destroy;
static roundtrip_fn roundtrip;
static dispatch_pending_fn dispatch_pending;
static dispatch_queue_fn dispatch_queue;
static wrapper_fn create_wrapper;
static wrapper_destroy_fn destroy_wrapper;
static set_queue_fn set_queue;
static flush_fn display_flush;
static const struct wl_interface *registry_interface;
static const struct wl_interface *surface_interface;
static const struct wl_interface *callback_interface;
static const struct wl_interface sync_surface_interface;
static const struct wl_interface *sync_manager_types[] = { &sync_surface_interface, 0 };
static int resolved;

static void *sym(const char *name) { return mv_wayland_symbol(name); }
static int resolve(void) {
    if (resolved) return marshal && proxy_version && add_listener && proxy_destroy &&
        queue_create && queue_destroy && roundtrip && dispatch_pending && dispatch_queue && create_wrapper &&
        destroy_wrapper && set_queue && display_flush && registry_interface && surface_interface && callback_interface;
    resolved = 1;
    marshal=(marshal_fn)sym("wl_proxy_marshal_flags");
    proxy_version=(version_fn)sym("wl_proxy_get_version");
    add_listener=(listener_fn)sym("wl_proxy_add_listener");
    proxy_destroy=(destroy_fn)sym("wl_proxy_destroy");
    queue_create=(queue_create_fn)sym("wl_display_create_queue");
    queue_destroy=(queue_destroy_fn)sym("wl_event_queue_destroy");
    roundtrip=(roundtrip_fn)sym("wl_display_roundtrip_queue");
    dispatch_pending=(dispatch_pending_fn)sym("wl_display_dispatch_queue_pending");
    dispatch_queue=(dispatch_queue_fn)sym("wl_display_dispatch_queue");
    create_wrapper=(wrapper_fn)sym("wl_proxy_create_wrapper");
    destroy_wrapper=(wrapper_destroy_fn)sym("wl_proxy_wrapper_destroy");
    set_queue=(set_queue_fn)sym("wl_proxy_set_queue");
    display_flush=(flush_fn)sym("wl_display_flush");
    registry_interface=(const struct wl_interface *)sym("wl_registry_interface");
    surface_interface=(const struct wl_interface *)sym("wl_surface_interface");
    callback_interface=(const struct wl_interface *)sym("wl_callback_interface");
    sync_manager_types[1]=surface_interface;
    return marshal && proxy_version && add_listener && proxy_destroy && queue_create &&
        queue_destroy && roundtrip && dispatch_pending && dispatch_queue && create_wrapper && destroy_wrapper &&
        set_queue && display_flush && registry_interface && surface_interface && callback_interface;
}

static const struct wl_interface params_interface, buffer_interface;
static const struct wl_interface ahb_manager_interface;
static const struct wl_interface *ahb_create_types[] = { &buffer_interface };
static const struct wl_message ahb_manager_methods[] = {
    {"destroy", "1", 0}, {"create_buffer", "nhuuu", ahb_create_types},
};
static const struct wl_interface ahb_manager_interface = {
    "maton_ahb_buffer_manager_v1", 1, 2, ahb_manager_methods, 0, 0,
};
static const struct wl_message sync_manager_methods[] = {
    {"destroy", "1", 0}, {"get_surface_sync", "no", sync_manager_types},
};
static const struct wl_message sync_surface_methods[] = {
    {"destroy", "1", 0}, {"set_acquire_fence", "h", 0},
};
static const struct wl_interface sync_surface_interface = {
    "maton_sync_surface_v1", 1, 2, sync_surface_methods, 0, 0,
};
static const struct wl_interface sync_manager_interface = {
    "maton_sync_file_v1", 1, 2, sync_manager_methods, 0, 0,
};
static const struct wl_interface *dmabuf_params_types[] = { &params_interface };
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
    {"create", "iiuu", 0},
    {"create_immed", "niiuu", params_create_immed_types},
};
static const struct wl_message params_events[] = {
    {"created", "n", params_created_types}, {"failed", "", 0},
};
static const struct wl_message buffer_methods[] = {{"destroy", "", 0}};
static const struct wl_message buffer_events[] = {{"release", "", 0}};
static const struct wl_interface params_interface = {
    "zwp_linux_buffer_params_v1", 5, 4, params_methods, 2, params_events,
};
static const struct wl_interface buffer_interface = {
    "wl_buffer", 1, 1, buffer_methods, 1, buffer_events,
};
static const struct wl_interface dmabuf_interface = {
    "zwp_linux_dmabuf_v1", 4, 2, dmabuf_methods, 2, dmabuf_events,
};
static void registry_global(void *data, struct wl_proxy *registry, uint32_t name,
        const char *interface, uint32_t version) {
    (void)registry;
    struct mv_wl_dmabuf *d=data;
    if (!interface) return;
    if (!d->global_name && !strcmp(interface,"zwp_linux_dmabuf_v1")) {
        d->global_name=name; d->version=version>4?4:(int)version;
    } else if (!d->sync_global_name && !strcmp(interface,"maton_sync_file_v1")) {
        d->sync_global_name=name; d->sync_version=version>1?1:(int)version;
    } else if (!d->ahb_global_name && !strcmp(interface,"maton_ahb_buffer_manager_v1")) {
        d->ahb_global_name=name; d->ahb_version=version>1?1:(int)version;
    }
}
static void registry_remove(void *data, struct wl_proxy *registry, uint32_t name) {
    (void)registry; struct mv_wl_dmabuf *d=data;
    if (d->global_name==name) { d->global_name=0; d->version=0; }
    if (d->sync_global_name==name) { d->sync_global_name=0; d->sync_version=0; }
    if (d->ahb_global_name==name) { d->ahb_global_name=0; d->ahb_version=0; }
}
static void (*const registry_listener[])(void) = {
    (void (*)(void))registry_global, (void (*)(void))registry_remove,
};
static void buffer_release(void *data, struct wl_proxy *proxy) {
    (void)proxy; ((struct mv_wl_buffer *)data)->released=1;
}
static void (*const buffer_listener[])(void) = {(void (*)(void))buffer_release};
static void frame_done(void *data, struct wl_proxy *proxy, uint32_t time) {
    (void)proxy; (void)time; ((struct mv_wl_sync_surface *)data)->frame_done=1;
}
static void (*const frame_listener[])(void) = {(void (*)(void))frame_done};

struct mv_wl_dmabuf *mv_wl_dmabuf_create(struct wl_display *display) {
    if (!display || !resolve()) return 0;
    struct mv_wl_dmabuf *d=mv_alloc(sizeof(*d)); if (!d) return 0;
    memset(d,0,sizeof(*d)); d->display=display;
    d->queue=queue_create(display); if (!d->queue) goto fail;
    struct wl_proxy *display_wrapper=create_wrapper((struct wl_proxy *)display);
    if (!display_wrapper) goto fail;
    set_queue(display_wrapper,d->queue);
    d->registry=marshal(display_wrapper,1,registry_interface,1,0,(void *)0);
    destroy_wrapper(display_wrapper); if (!d->registry) goto fail;
    set_queue(d->registry,d->queue);
    if (add_listener(d->registry,(void (**)(void))registry_listener,d)) goto fail;
    if (roundtrip(display,d->queue)<0 || ((!d->global_name || d->version<2) && !d->ahb_global_name)) goto fail;
    if (d->global_name && d->version>=2) {
        d->proxy=marshal(d->registry,0,&dmabuf_interface,(uint32_t)d->version,0,
                d->global_name,dmabuf_interface.name,(uint32_t)d->version,(void *)0);
        if (!d->proxy) goto fail;
        set_queue(d->proxy,d->queue);
    }
    if (d->ahb_global_name && d->ahb_version>0) {
        d->ahb_manager=marshal(d->registry,0,&ahb_manager_interface,(uint32_t)d->ahb_version,0,
                d->ahb_global_name,ahb_manager_interface.name,(uint32_t)d->ahb_version,(void *)0);
        if (d->ahb_manager) set_queue(d->ahb_manager,d->queue);
    }
    if(d->sync_global_name && d->sync_version>0) {
        d->sync_manager=marshal(d->registry,0,&sync_manager_interface,(uint32_t)d->sync_version,0,
                d->sync_global_name,sync_manager_interface.name,(uint32_t)d->sync_version,(void *)0);
        if(d->sync_manager)set_queue(d->sync_manager,d->queue);
    }
    if(d->proxy) { d->wrapper=create_wrapper(d->proxy); if (!d->wrapper) goto fail; set_queue(d->wrapper,d->queue); }
    return d;
fail:
    mv_wl_dmabuf_destroy(d); return 0;
}
void mv_wl_dmabuf_destroy(struct mv_wl_dmabuf *d) {
    if (!d) return;
    if (d->wrapper) destroy_wrapper(d->wrapper);
    if (d->proxy) marshal(d->proxy,0,0,proxy_version(d->proxy),1);
    if (d->sync_manager) marshal(d->sync_manager,0,0,proxy_version(d->sync_manager),1);
    if (d->ahb_manager) marshal(d->ahb_manager,0,0,proxy_version(d->ahb_manager),1);
    if (d->registry) proxy_destroy(d->registry);
    if (d->queue) queue_destroy(d->queue);
    mv_free(d);
}
int mv_wl_dmabuf_version(const struct mv_wl_dmabuf *d) { return d?d->version:0; }
int mv_wl_has_ahb_buffer(const struct mv_wl_dmabuf *d) { return d && d->ahb_manager; }
int mv_wl_dmabuf_dispatch(struct mv_wl_dmabuf *d) {
    if (!d || !d->display || !d->queue || !dispatch_pending) return -1;
    return dispatch_pending(d->display,d->queue);
}
int mv_wl_dmabuf_dispatch_one(struct mv_wl_dmabuf *d) {
    if(!d||!d->display||!d->queue||!dispatch_queue)return -1;
    return dispatch_queue(d->display,d->queue);
}

struct mv_wl_buffer *mv_wl_dmabuf_create_buffer(struct mv_wl_dmabuf *d,
        uint32_t width,uint32_t height,uint32_t format,const struct mv_wl_plane *planes,uint32_t n) {
    if (!d || !d->proxy || !d->wrapper || !width || !height || !planes || !n || n>4) return 0;
    struct wl_proxy *params=marshal(d->wrapper,1,&params_interface,
            (uint32_t)d->version,0,(void *)0);
    if (!params) return 0;
    set_queue(params,d->queue);
    for (uint32_t i=0;i<n;i++) {
        if (planes[i].fd<0 || planes[i].index>=4 || !planes[i].stride) {
            marshal(params,0,0,proxy_version(params),1); return 0;
        }
        marshal(params,1,0,proxy_version(params),0,planes[i].fd,planes[i].index,
                planes[i].offset,planes[i].stride,(uint32_t)(planes[i].modifier>>32),
                (uint32_t)planes[i].modifier);
    }
    struct wl_proxy *raw=marshal(params,3,&buffer_interface,1,0,(void *)0,
            (int32_t)width,(int32_t)height,format,0);
    marshal(params,0,0,proxy_version(params),1);
    if (!raw) return 0;
    set_queue(raw,d->queue);
    struct mv_wl_buffer *b=mv_alloc(sizeof(*b));
    if (!b) { marshal(raw,0,0,proxy_version(raw),1); return 0; }
    b->proxy=raw; b->released=0;
    if (add_listener(raw,(void (**)(void))buffer_listener,b)) {
        marshal(raw,0,0,proxy_version(raw),1); mv_free(b); return 0;
    }
    return b;
}
struct mv_wl_buffer *mv_wl_ahb_create_buffer(struct mv_wl_dmabuf *d,
        uint32_t width,uint32_t height,uint32_t format,int transport_fd) {
    if(!d||!d->ahb_manager||!width||!height||transport_fd<0)return 0;
    struct wl_proxy *raw=marshal(d->ahb_manager,1,&buffer_interface,1,0,(void *)0,
            transport_fd,width,height,format);
    if(!raw)return 0;
    set_queue(raw,d->queue);
    struct mv_wl_buffer *b=mv_alloc(sizeof(*b));
    if(!b){marshal(raw,0,0,proxy_version(raw),1);return 0;}
    b->proxy=raw;b->released=0;
    if(add_listener(raw,(void (**)(void))buffer_listener,b)){
        marshal(raw,0,0,proxy_version(raw),1);mv_free(b);return 0;
    }
    return b;
}
void mv_wl_buffer_destroy(struct mv_wl_buffer *b) {
    if (!b) return;
    if (b->proxy) marshal(b->proxy,0,0,proxy_version(b->proxy),1);
    mv_free(b);
}
int mv_wl_buffer_released(const struct mv_wl_buffer *b) { return b && b->released; }
void mv_wl_buffer_clear_released(struct mv_wl_buffer *b) { if (b) b->released=0; }
struct mv_wl_sync_surface *mv_wl_sync_surface_create(struct mv_wl_dmabuf *d,struct wl_surface *surface) {
    if(!d||!d->sync_manager||!surface)return 0;
    struct wl_proxy *proxy=marshal(d->sync_manager,1,&sync_surface_interface,
            (uint32_t)d->sync_version,0,(void *)0,surface);
    if(!proxy)return 0;set_queue(proxy,d->queue);
    struct mv_wl_sync_surface *sync=mv_alloc(sizeof(*sync));
    if(!sync){marshal(proxy,0,0,proxy_version(proxy),1);return 0;}
    memset(sync,0,sizeof(*sync));
    sync->proxy=proxy;sync->display=d->display;sync->queue=d->queue;return sync;
}
void mv_wl_sync_surface_destroy(struct mv_wl_sync_surface *sync) {
    if(!sync)return;if(sync->frame)marshal(sync->frame,0,0,proxy_version(sync->frame),1);if(sync->proxy)marshal(sync->proxy,0,0,proxy_version(sync->proxy),1);mv_free(sync);
}
int mv_wl_surface_present(struct mv_wl_sync_surface *sync,struct wl_surface *surface,
        struct mv_wl_buffer *buffer,int fence_fd) {
    if(!sync||!sync->proxy||!surface||!buffer||!buffer->proxy||fence_fd<0)return -1;
    if(sync->frame){marshal(sync->frame,0,0,proxy_version(sync->frame),1);sync->frame=0;}
    sync->frame_done=0;
    sync->frame=marshal((struct wl_proxy *)surface,3,callback_interface,1,0,(void *)0);
    if(!sync->frame)return -1;
    set_queue(sync->frame,sync->queue);
    if(add_listener(sync->frame,(void (**)(void))frame_listener,sync)){marshal(sync->frame,0,0,proxy_version(sync->frame),1);sync->frame=0;return -1;}
    marshal(sync->proxy,1,0,proxy_version(sync->proxy),0,fence_fd);
    uint32_t version=proxy_version((struct wl_proxy *)surface);
    marshal((struct wl_proxy *)surface,1,0,version,0,buffer->proxy,0,0);
    if(version>=4)marshal((struct wl_proxy *)surface,9,0,version,0,0,0,0,0);
    else marshal((struct wl_proxy *)surface,2,0,version,0,0,0,0,0);
    marshal((struct wl_proxy *)surface,6,0,version,0);
    return display_flush(sync->display);
}
int mv_wl_sync_wait_frame(struct mv_wl_sync_surface *sync) {
    if(!sync||!sync->display||!sync->frame)return -1;
    while(!sync->frame_done)if(dispatch_queue(sync->display,sync->queue)<0)return -1;
    marshal(sync->frame,0,0,proxy_version(sync->frame),1);sync->frame=0;return 0;
}
