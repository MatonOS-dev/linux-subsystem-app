#define _GNU_SOURCE
#include <android/sharedmem.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include "xdg-shell-client-protocol.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct Demo {
  struct wl_display *display;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct wl_keyboard *keyboard;
  struct xdg_wm_base *wm;
  struct wl_surface *surface;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *toplevel;
  struct wl_buffer *buffer;
  uint32_t *pixels;
  int width, height, configured, dirty, color;
  size_t mapped_bytes;
};

static void draw(struct Demo *d) {
  if (!d->pixels || !d->configured) return;
  uint32_t bg[] = {0xff24324a, 0xff244838, 0xff493047, 0xff47412b};
  uint32_t fg = bg[d->color & 3];
  for (int y = 0; y < d->height; ++y) for (int x = 0; x < d->width; ++x) {
    int border = x < 7 || y < 7 || x >= d->width - 7 || y >= d->height - 7;
    d->pixels[y * d->width + x] = border ? 0xff62d8c6 : fg;
  }
  d->dirty = 0;
  wl_surface_attach(d->surface, d->buffer, 0, 0);
  wl_surface_damage_buffer(d->surface, 0, 0, d->width, d->height);
  wl_surface_commit(d->surface);
}

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) { (void)data; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};
static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
  struct Demo *d = data; xdg_surface_ack_configure(surface, serial); d->configured = 1; draw(d);
}
static const struct xdg_surface_listener surface_listener = {.configure = surface_configure};
static void top_configure(void *data, struct xdg_toplevel *top, int32_t w, int32_t h, struct wl_array *states) {
  struct Demo *d = data; (void)top; (void)states;
  if (w > 0 && h > 0 && (w != d->width || h != d->height)) {
    d->width = w; d->height = h;
    if (d->buffer) { wl_buffer_destroy(d->buffer); d->buffer = NULL; }
    if (d->pixels) { munmap(d->pixels, d->mapped_bytes); d->pixels = NULL; d->mapped_bytes = 0; }
    d->dirty = 1;
  }
}
static void top_close(void *data, struct xdg_toplevel *top) { (void)top; ((struct Demo *)data)->configured = -1; }
static void top_bounds(void *data, struct xdg_toplevel *top, int32_t w, int32_t h) { (void)data; (void)top; (void)w; (void)h; }
static void top_caps(void *data, struct xdg_toplevel *top, struct wl_array *caps) { (void)data; (void)top; (void)caps; }
static const struct xdg_toplevel_listener top_listener = {.configure = top_configure, .close = top_close, .configure_bounds = top_bounds, .wm_capabilities = top_caps};

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) { (void)data;(void)p;(void)serial;(void)s;(void)x;(void)y; }
static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) { (void)data;(void)p;(void)serial;(void)s; }
static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y) { struct Demo *d=data;(void)p;(void)time;(void)x;(void)y; d->color++; draw(d); }
static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) { struct Demo *d=data;(void)p;(void)serial;(void)time;(void)button;(void)state; d->color++; draw(d); }
static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value) { (void)data;(void)p;(void)time;(void)axis;(void)value; }
static void pointer_frame(void *data, struct wl_pointer *p) { (void)data;(void)p; }
static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t source) { (void)data;(void)p;(void)source; }
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis) { (void)data;(void)p;(void)time;(void)axis; }
static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete) { (void)data;(void)p;(void)axis;(void)discrete; }
static void pointer_axis_value120(void *data, struct wl_pointer *p, uint32_t axis, int32_t value) { (void)data;(void)p;(void)axis;(void)value; }
static void pointer_axis_relative(void *data, struct wl_pointer *p, uint32_t axis, uint32_t direction) { (void)data;(void)p;(void)axis;(void)direction; }
static const struct wl_pointer_listener pointer_listener = {
  .enter=pointer_enter,.leave=pointer_leave,.motion=pointer_motion,.button=pointer_button,.axis=pointer_axis,
  .frame=pointer_frame,.axis_source=pointer_axis_source,.axis_stop=pointer_axis_stop,.axis_discrete=pointer_axis_discrete,
  .axis_value120=pointer_axis_value120,.axis_relative_direction=pointer_axis_relative
};
static void keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size) { (void)data;(void)k;(void)format;(void)size; close(fd); }
static void key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t code, uint32_t state) { struct Demo *d=data;(void)k;(void)serial;(void)time;(void)code;(void)state; d->color++; draw(d); }
static void keyboard_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) { (void)data;(void)k;(void)serial;(void)s;(void)keys; }
static void keyboard_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) { (void)data;(void)k;(void)serial;(void)s; }
static void modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) { (void)data;(void)k;(void)serial;(void)dep;(void)lat;(void)lock;(void)group; }
static void repeat_info(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay) { (void)data;(void)k;(void)rate;(void)delay; }
static const struct wl_keyboard_listener keyboard_listener = {.keymap=keymap,.enter=keyboard_enter,.leave=keyboard_leave,.key=key,.modifiers=modifiers,.repeat_info=repeat_info};
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
  struct Demo *d=data;
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->pointer) { d->pointer=wl_seat_get_pointer(seat); wl_pointer_add_listener(d->pointer,&pointer_listener,d); }
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) { d->keyboard=wl_seat_get_keyboard(seat); wl_keyboard_add_listener(d->keyboard,&keyboard_listener,d); }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name) { (void)data;(void)seat;(void)name; }
static const struct wl_seat_listener seat_listener = {.capabilities=seat_capabilities,.name=seat_name};

static int make_buffer(struct Demo *d) {
  size_t bytes=(size_t)d->width*d->height*4;
  int fd=ASharedMemory_create("wayland-demo",bytes);
  if(fd<0) return -1;
  d->pixels=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(d->pixels==MAP_FAILED){d->pixels=NULL;close(fd);return -1;}
  d->mapped_bytes=bytes;
  struct wl_shm_pool *pool=wl_shm_create_pool(d->shm,fd,(int32_t)bytes);
  d->buffer=wl_shm_pool_create_buffer(pool,0,d->width,d->height,d->width*4,WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool); close(fd); return d->buffer?0:-1;
}

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *iface, uint32_t version) {
  struct Demo *d=data;
  if(!strcmp(iface,wl_compositor_interface.name)) d->compositor=wl_registry_bind(registry,name,&wl_compositor_interface,version<6?version:6);
  else if(!strcmp(iface,wl_shm_interface.name)) d->shm=wl_registry_bind(registry,name,&wl_shm_interface,1);
  else if(!strcmp(iface,xdg_wm_base_interface.name)) { d->wm=wl_registry_bind(registry,name,&xdg_wm_base_interface,version<6?version:6); xdg_wm_base_add_listener(d->wm,&wm_listener,d); }
  else if(!strcmp(iface,wl_seat_interface.name)) { d->seat=wl_registry_bind(registry,name,&wl_seat_interface,version<8?version:8); wl_seat_add_listener(d->seat,&seat_listener,d); }
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name) { (void)data;(void)registry;(void)name; }
static const struct wl_registry_listener registry_listener={.global=global,.global_remove=global_remove};

void *maton_wayland_test_client(void *unused) {
  (void)unused;
  struct Demo d={.width=640,.height=400,.dirty=1};
  d.display=wl_display_connect(NULL);
  if(!d.display) return NULL;
  struct wl_registry *registry=wl_display_get_registry(d.display);
  wl_registry_add_listener(registry,&registry_listener,&d);
  wl_display_roundtrip(d.display);
  if(!d.compositor||!d.shm||!d.wm) goto finish;
  d.surface=wl_compositor_create_surface(d.compositor);
  d.xdg_surface=xdg_wm_base_get_xdg_surface(d.wm,d.surface);
  xdg_surface_add_listener(d.xdg_surface,&surface_listener,&d);
  d.toplevel=xdg_surface_get_toplevel(d.xdg_surface);
  xdg_toplevel_add_listener(d.toplevel,&top_listener,&d);
  xdg_toplevel_set_title(d.toplevel,"MatonOS Wayland test client");
  xdg_toplevel_set_app_id(d.toplevel,"org.matonos.wayland-test");
  wl_surface_commit(d.surface);
  while(d.configured>=0 && wl_display_dispatch(d.display)>=0) {
    if(d.dirty) { if(!d.buffer && make_buffer(&d)!=0) break; draw(&d); }
  }
finish:
  if(d.pointer) wl_pointer_destroy(d.pointer); if(d.keyboard) wl_keyboard_destroy(d.keyboard);
  if(d.toplevel) xdg_toplevel_destroy(d.toplevel); if(d.xdg_surface) xdg_surface_destroy(d.xdg_surface);
  if(d.surface) wl_surface_destroy(d.surface); if(d.buffer) wl_buffer_destroy(d.buffer);
  if(d.pixels) munmap(d.pixels,d.mapped_bytes);
  if(d.seat) wl_seat_destroy(d.seat); if(d.wm) xdg_wm_base_destroy(d.wm);
  if(d.shm) wl_shm_destroy(d.shm); if(d.compositor) wl_compositor_destroy(d.compositor);
  wl_registry_destroy(registry); wl_display_disconnect(d.display); return NULL;
}
