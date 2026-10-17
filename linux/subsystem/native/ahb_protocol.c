#include "ahb_protocol.h"
#include "maton-ahb-v1-server-protocol.h"
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server-protocol.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>

#define AHB_USAGE (AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY)
struct Session {
  struct wl_resource* resource;
  struct wl_listener surface_destroy;
  int fd;
  uint32_t sequence, formats;
  size_t refs;
  bool alive;
};
struct Buffer {
  struct wlr_buffer base;
  struct wl_resource* resource;
  struct wl_listener release;
  struct Session* session;
  AHardwareBuffer* ahb;
};
static void session_unref(struct Session* s) {
  if(--s->refs)return;
  free(s);
}
static void buffer_destroy(struct wlr_buffer* base) {
  struct Buffer* b=(struct Buffer*)base;
  wl_list_remove(&b->release.link);
  AHardwareBuffer_release(b->ahb);
  session_unref(b->session);
  wlr_buffer_finish(base);free(b);
}
static const struct wlr_buffer_impl buffer_impl={.destroy=buffer_destroy};
AHardwareBuffer* maton_ahb_protocol_buffer(struct wlr_buffer* base) {
  if(!base)return NULL;
  struct wlr_client_buffer* client=wlr_client_buffer_get(base);
  if(client)base=client->source;
  return base && base->impl==&buffer_impl?((struct Buffer*)base)->ahb:NULL;
}
static void released(struct wl_listener* listener, void* unused) {
  (void)unused;
  struct Buffer* b=wl_container_of(listener,b,release);
  if(b->resource)wl_buffer_send_release(b->resource);
}
static void destroy_resource(struct wl_client* client, struct wl_resource* resource) {
  (void)client;wl_resource_destroy(resource);
}
static const struct wl_buffer_interface buffer_requests={.destroy=destroy_resource};
static void buffer_resource_destroy(struct wl_resource* resource) {
  struct Buffer* b=wl_resource_get_user_data(resource);
  b->resource=NULL;wlr_buffer_drop(&b->base);
}
static bool is_buffer(struct wl_resource* resource) {
  return wl_resource_instance_of(resource,&wl_buffer_interface,&buffer_requests);
}
static struct wlr_buffer* from_resource(struct wl_resource* resource) {
  return &((struct Buffer*)wl_resource_get_user_data(resource))->base;
}
static const struct wlr_buffer_resource_interface resource_interface={
  .name="maton-ahb-v1",.is_instance=is_buffer,.from_resource=from_resource,
};
static void surface_destroy(struct wl_listener* listener, void* unused) {
  (void)unused;
  struct Session* s=wl_container_of(listener,s,surface_destroy);
  wl_list_remove(&s->surface_destroy.link);wl_list_init(&s->surface_destroy.link);
  s->alive=false;
  if(s->fd>=0)close(s->fd);
  s->fd=-1;
}
static void session_destroy(struct wl_resource* resource) {
  struct Session* s=wl_resource_get_user_data(resource);
  wl_list_remove(&s->surface_destroy.link);
  if(s->fd>=0)close(s->fd);
  s->resource=NULL;s->alive=false;session_unref(s);
}
static void fail(struct Session* s,uint32_t request,uint32_t reason) {
  maton_ahb_session_v1_send_failed(s->resource,request,reason);
}
/* The public nativewindow sender uses sendmsg(flags=0), so EPIPE can also
 * deliver SIGPIPE. Contain that signal on this thread without changing the
 * process handler or swallowing a signal that was already pending. */
static int send_handle(AHardwareBuffer* buffer,int fd) {
  sigset_t blocked,previous,before;
  sigemptyset(&blocked);sigaddset(&blocked,SIGPIPE);
  if(pthread_sigmask(SIG_BLOCK,&blocked,&previous))return -1;
  if(sigpending(&before)) { pthread_sigmask(SIG_SETMASK,&previous,NULL);return -1; }
  int result=AHardwareBuffer_sendHandleToUnixSocket(buffer,fd);
  if(!sigismember(&before,SIGPIPE)) {
    struct timespec zero={0};
    while(sigtimedwait(&blocked,NULL,&zero)<0 && errno==EINTR){}
  }
  pthread_sigmask(SIG_SETMASK,&previous,NULL);
  return result;
}
static void create_buffer(struct wl_client* client,struct wl_resource* resource,uint32_t request,uint32_t width,uint32_t height,uint32_t format) {
  struct Session* s=wl_resource_get_user_data(resource);
  if(!request || request<=s->sequence) {
    wl_resource_post_error(resource,0,"AHB request sequence must increase");return;
  }
  s->sequence=request;
  if(!s->alive){fail(s,request,MATON_AHB_SESSION_V1_FAILURE_SURFACE_LOST);return;}
  if(!width || !height ||
      (format!=1 && format!=5) || !(s->formats&(1u<<format))) {
    fail(s,request,MATON_AHB_SESSION_V1_FAILURE_INVALID);return;
  }
  if(s->refs==SIZE_MAX){fail(s,request,MATON_AHB_SESSION_V1_FAILURE_ALLOCATION);return;}
  struct Buffer* b=calloc(1,sizeof(*b));
  if(!b){wl_client_post_no_memory(client);return;}
  AHardwareBuffer_Desc desc={.width=width,.height=height,.layers=1,.format=format,.usage=AHB_USAGE};
  if(AHardwareBuffer_allocate(&desc,&b->ahb) || !b->ahb) {
    free(b);fail(s,request,MATON_AHB_SESSION_V1_FAILURE_ALLOCATION);return;
  }
  AHardwareBuffer_Desc actual;AHardwareBuffer_describe(b->ahb,&actual);
  /* wlroots dimensions are signed int; never truncate a driver extent. */
  if(actual.width!=width || actual.height!=height || actual.layers!=1 || actual.format!=format ||
      (actual.usage&AHB_USAGE)!=AHB_USAGE || actual.stride<width || width>INT32_MAX || height>INT32_MAX) {
    AHardwareBuffer_release(b->ahb);free(b);fail(s,request,MATON_AHB_SESSION_V1_FAILURE_ALLOCATION);return;
  }
  b->resource=wl_resource_create(client,&wl_buffer_interface,1,0);
  if(!b->resource){AHardwareBuffer_release(b->ahb);free(b);wl_client_post_no_memory(client);return;}
  /* Nonblocking SEQPACKET: a complete opaque handle or failure, no partial
   * stream framing and no wait for a stopped/malicious client. */
  if(send_handle(b->ahb,s->fd)) {
    wl_resource_destroy(b->resource);AHardwareBuffer_release(b->ahb);free(b);
    close(s->fd);s->fd=-1;s->alive=false;
    fail(s,request,MATON_AHB_SESSION_V1_FAILURE_TRANSPORT);return;
  }
  b->session=s;s->refs++;
  wlr_buffer_init(&b->base,&buffer_impl,(int)width,(int)height);
  b->release.notify=released;wl_signal_add(&b->base.events.release,&b->release);
  wl_resource_set_implementation(b->resource,&buffer_requests,b,buffer_resource_destroy);
  maton_ahb_session_v1_send_allocated(resource,request,b->resource,(uint32_t)actual.usage,(uint32_t)(actual.usage>>32));
}
static const struct maton_ahb_session_v1_interface session_requests={.destroy=destroy_resource,.create_buffer=create_buffer};
static void get_session(struct wl_client* client,struct wl_resource* resource,uint32_t id,struct wl_resource* surface) {
  /* Wayland object arguments already belong to this client. Check explicitly
   * before accepting a lifetime authority. No numeric surface ID is reused on
   * another connection. */
  if(wl_resource_get_client(surface)!=client){wl_resource_post_error(resource,0,"foreign surface");return;}
  struct wlr_surface* target=wlr_surface_from_resource(surface);
  if(!target){wl_resource_post_error(resource,0,"invalid surface");return;}
  int sockets[2];
  if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0,sockets)) {
    wl_client_post_implementation_error(client,"AHB socketpair unavailable");return;
  }
  struct Session* s=calloc(1,sizeof(*s));
  if(!s){close(sockets[0]);close(sockets[1]);wl_client_post_no_memory(client);return;}
  s->resource=wl_resource_create(client,&maton_ahb_session_v1_interface,1,id);
  if(!s->resource){close(sockets[0]);close(sockets[1]);free(s);wl_client_post_no_memory(client);return;}
  s->fd=sockets[0];s->alive=true;s->refs=1;
  s->surface_destroy.notify=surface_destroy;wl_signal_add(&target->events.destroy,&s->surface_destroy);
  wl_resource_set_implementation(s->resource,&session_requests,s,session_destroy);
  maton_ahb_session_v1_send_transport(s->resource,sockets[1],0,0,0);
  close(sockets[1]);
  for(uint32_t format=1;format<=5;format+=4) {
    AHardwareBuffer_Desc desc={.width=64,.height=64,.layers=1,.format=format,.usage=AHB_USAGE};
    if(AHardwareBuffer_isSupported(&desc)){s->formats|=1u<<format;maton_ahb_session_v1_send_format(s->resource,format);}
  }
  maton_ahb_session_v1_send_ready(s->resource);
}
static const struct maton_ahb_manager_v1_interface manager_requests={.destroy=destroy_resource,.get_session=get_session};
static void bind_manager(struct wl_client* client,void* data,uint32_t version,uint32_t id) {
  (void)data;
  struct wl_resource* r=wl_resource_create(client,&maton_ahb_manager_v1_interface,(int)version,id);
  if(!r){wl_client_post_no_memory(client);return;}
  wl_resource_set_implementation(r,&manager_requests,NULL,NULL);
}
bool maton_ahb_protocol_create(struct wl_display* display) {
  wlr_buffer_register_resource_interface(&resource_interface);
  return wl_global_create(display,&maton_ahb_manager_v1_interface,1,NULL,bind_manager)!=NULL;
}
