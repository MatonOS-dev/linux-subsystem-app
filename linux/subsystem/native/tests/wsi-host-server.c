/* Minimal SHM/xdg test peer, not the production wlroots compositor.
 * Exercises real host Vulkan WSI and the resident syscall proxy. */
#include <wayland-server.h>
#include "xdg-shell-server-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct Surface { struct wl_resource *xdg,*top,*buffer; struct wl_list frames; int configured; };
struct Frame { struct wl_resource *resource; struct wl_list link; };
static struct wl_display *display;
static unsigned buffers;
static void destroy(struct wl_client *c,struct wl_resource *r){(void)c;wl_resource_destroy(r);}
static void ignored(struct wl_client *c,struct wl_resource *r){(void)c;(void)r;}
static void string(struct wl_client *c,struct wl_resource *r,const char *s){(void)c;(void)r;(void)s;}
static void size(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y){(void)c;(void)r;(void)x;(void)y;}
static void region(struct wl_client *c,struct wl_resource *r,struct wl_resource *v){(void)c;(void)r;(void)v;}
static void integer(struct wl_client *c,struct wl_resource *r,int32_t v){(void)c;(void)r;(void)v;}
static void damage(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y,int32_t w,int32_t h){(void)c;(void)r;(void)x;(void)y;(void)w;(void)h;}
static void attach(struct wl_client *c,struct wl_resource *r,struct wl_resource *b,int32_t x,int32_t y){(void)c;(void)x;(void)y;((struct Surface*)wl_resource_get_user_data(r))->buffer=b;}
static void frame_free(struct wl_resource *r){struct Frame *f=wl_resource_get_user_data(r);wl_list_remove(&f->link);free(f);}
static void frame(struct wl_client *c,struct wl_resource *r,uint32_t id){struct Surface *s=wl_resource_get_user_data(r);struct Frame *f=calloc(1,sizeof(*f));f->resource=wl_resource_create(c,&wl_callback_interface,1,id);wl_resource_set_implementation(f->resource,NULL,f,frame_free);wl_list_insert(&s->frames,&f->link);}
static void commit(struct wl_client *c,struct wl_resource *r){(void)c;struct Surface *s=wl_resource_get_user_data(r);
 if(s->xdg&&!s->configured){struct wl_array states;wl_array_init(&states);xdg_toplevel_send_configure(s->top,320,240,&states);xdg_surface_send_configure(s->xdg,wl_display_next_serial(display));s->configured=1;return;}
 if(s->buffer){buffers++;struct wl_shm_buffer *b=wl_shm_buffer_get(s->buffer);if(!b)abort();wl_shm_buffer_begin_access(b);volatile unsigned pixel=*(unsigned*)wl_shm_buffer_get_data(b);(void)pixel;wl_shm_buffer_end_access(b);wl_buffer_send_release(s->buffer);s->buffer=NULL;fprintf(stderr,"SHM present %u\n",buffers);}
 struct Frame *f,*tmp;wl_list_for_each_safe(f,tmp,&s->frames,link){wl_callback_send_done(f->resource,buffers*16);wl_resource_destroy(f->resource);}
}
static void surface_free(struct wl_resource *r){struct Surface *s=wl_resource_get_user_data(r);struct Frame *f,*tmp;wl_list_for_each_safe(f,tmp,&s->frames,link)wl_resource_destroy(f->resource);free(s);}
static const struct wl_surface_interface surface_impl={.destroy=destroy,.attach=attach,.damage=damage,.frame=frame,.set_opaque_region=region,.set_input_region=region,.commit=commit,.set_buffer_transform=integer,.set_buffer_scale=integer,.damage_buffer=damage};
static void create_surface(struct wl_client *c,struct wl_resource *r,uint32_t id){struct Surface *s=calloc(1,sizeof(*s));wl_list_init(&s->frames);struct wl_resource *v=wl_resource_create(c,&wl_surface_interface,wl_resource_get_version(r),id);wl_resource_set_implementation(v,&surface_impl,s,surface_free);}
static const struct wl_compositor_interface compositor_impl={.create_surface=create_surface};
static const struct xdg_toplevel_interface top_impl={.destroy=destroy,.set_parent=region,.set_title=string,.set_app_id=string,.set_max_size=size,.set_min_size=size,.set_maximized=ignored,.unset_maximized=ignored,.set_fullscreen=region,.unset_fullscreen=ignored,.set_minimized=ignored};
static void get_top(struct wl_client *c,struct wl_resource *r,uint32_t id){struct Surface *s=wl_resource_get_user_data(r);s->top=wl_resource_create(c,&xdg_toplevel_interface,wl_resource_get_version(r),id);wl_resource_set_implementation(s->top,&top_impl,s,NULL);}
static void ack(struct wl_client *c,struct wl_resource *r,uint32_t serial){(void)c;(void)r;fprintf(stderr,"ACK %u\n",serial);}
static const struct xdg_surface_interface xdg_impl={.destroy=destroy,.get_toplevel=get_top,.set_window_geometry=damage,.ack_configure=ack};
static void get_xdg(struct wl_client *c,struct wl_resource *r,uint32_t id,struct wl_resource *v){struct Surface *s=wl_resource_get_user_data(v);s->xdg=wl_resource_create(c,&xdg_surface_interface,wl_resource_get_version(r),id);wl_resource_set_implementation(s->xdg,&xdg_impl,s,NULL);}
static void pong(struct wl_client *c,struct wl_resource *r,uint32_t v){(void)c;(void)r;(void)v;}
static const struct xdg_wm_base_interface shell_impl={.destroy=destroy,.get_xdg_surface=get_xdg,.pong=pong};
static void bind_compositor(struct wl_client *c,void *data,uint32_t version,uint32_t id){(void)data;struct wl_resource *r=wl_resource_create(c,&wl_compositor_interface,version,id);wl_resource_set_implementation(r,&compositor_impl,NULL,NULL);}
static void bind_shell(struct wl_client *c,void *data,uint32_t version,uint32_t id){(void)data;struct wl_resource *r=wl_resource_create(c,&xdg_wm_base_interface,version,id);wl_resource_set_implementation(r,&shell_impl,NULL,NULL);}
int main(int argc,char **argv){display=wl_display_create();if(!display||wl_display_init_shm(display)<0)return 1;wl_global_create(display,&wl_compositor_interface,4,NULL,bind_compositor);wl_global_create(display,&xdg_wm_base_interface,6,NULL,bind_shell);if(argc!=2||!wl_client_create(display,atoi(argv[1]))){perror("wl_client_create");return 1;}wl_display_run(display);return 0;}
