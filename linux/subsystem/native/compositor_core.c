#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif
#include "android_buffer.h"
#include "android_output.h"
#include "compositor_core.h"
#include "us_keymap.h"
#include "android_keymap.h"

#include <android/log.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/render/drm_format_set.h>
#include <dirent.h>
#include <math.h>
#include <sys/stat.h>
#include "dmabuf_import.h"
#include "ahb_buffer.h"
#include "maton_renderer.h"
#include "presenter.h"
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/xwayland/xwayland.h>
#include <wlr/xwayland/server.h>
#include <wlr/xwayland/shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include "pointer_buttons.h"
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/util/region.h>
#include "window_fit.h"
#include "xwayland_lifetime.h"
#include "short_socket_path.h"

extern void* maton_wayland_test_client(void*);

/* Route wlroots logging into logcat: stderr is lost in app processes, and
 * Xwayland spawn or XWM failures would otherwise be invisible. */
static void maton_wlr_log(enum wlr_log_importance importance,const char* fmt,va_list args) {
  /* wlroots orders SILENT < ERROR < INFO < DEBUG (higher is chattier). */
  int priority=importance<=WLR_ERROR?ANDROID_LOG_ERROR:importance==WLR_INFO?ANDROID_LOG_INFO:ANDROID_LOG_DEBUG;
  __android_log_vprint(priority,"MatonWLR",fmt,args);
}

/* Opt-in WSI handshake trace. Syscall logs cannot distinguish a Vulkan
 * roundtrip from an xdg configure wait. Do not log strings (titles, paths),
 * array contents, or fd contents; cap output even for a busy/malicious client.
 * The display owns this logger and destroys it on teardown. */
static void trace_wsi(void* data,enum wl_protocol_logger_type direction,
                      const struct wl_protocol_logger_message* message) {
  unsigned* count=data;
  if(*count>=8192||!__android_log_is_loggable(ANDROID_LOG_DEBUG,"MatonWSI",ANDROID_LOG_INFO))return;
  const char* interface=wl_resource_get_class(message->resource);
  if(strcmp(interface,"wl_display")&&strcmp(interface,"wl_registry")&&
      strcmp(interface,"wl_callback")&&strcmp(interface,"wl_surface")&&
      strcmp(interface,"wl_shm")&&strcmp(interface,"wl_shm_pool")&&
      strcmp(interface,"wl_buffer")&&strncmp(interface,"xdg_",4)&&
      strncmp(interface,"zxdg_",5)&&strncmp(interface,"zwp_linux_",10))return;
  /* Registry globals and shm/dmabuf format lists are long and fixed. */
  if(!strcmp(message->message->name,"global")||!strcmp(message->message->name,"format")||
      !strcmp(message->message->name,"modifier"))return;
  const char* signature=message->message->signature;
  while(*signature>='0'&&*signature<='9')signature++;
  if(*signature=='?')signature++;
  uint32_t first=0;
  if(message->arguments_count){
    if(*signature=='u')first=message->arguments[0].u;
    else if(*signature=='i')first=(uint32_t)message->arguments[0].i;
    else if(*signature=='o'&&message->arguments[0].o)
      first=wl_resource_get_id((struct wl_resource*)message->arguments[0].o);
  }
  pid_t pid=0;
  struct wl_client* client=wl_resource_get_client(message->resource);
  wl_client_get_credentials(client,&pid,NULL,NULL);
  __android_log_print(ANDROID_LOG_DEBUG,"MatonWSI",
      "pid=%d fd=%d %s %s@%u.%s argc=%d first=%u",pid,wl_client_get_fd(client),
      direction==WL_PROTOCOL_LOGGER_REQUEST?"request":"event",interface,
      wl_resource_get_id(message->resource),message->message->name,
      message->arguments_count,first);
  if(++*count==8192)__android_log_print(ANDROID_LOG_DEBUG,"MatonWSI","trace limit reached");
}

struct SocketSession {
  int id, fd;
  int directory_fd;
  char path[PATH_MAX];
  struct SocketSession* next;
};
struct XwaylandSession {
  int process_fd;
  pid_t process_pid;
  int session;
  uid_t uid;
  xcb_atom_t delete_window;
  struct wlr_xwayland* xwayland;
  struct wl_listener ready, new_surface, destroy;
  struct XwaylandSession* next;
};
enum CommandKind { CMD_STOPPED, CMD_ATTACH, CMD_DETACH, CMD_RESIZE, CMD_KEY, CMD_MOTION, CMD_RELEASE, CMD_CLOSE, CMD_STOP, CMD_ADD_SESSION, CMD_ADD_XWAYLAND, CMD_FULLSCREEN, CMD_RELATIVE, CMD_CAPTURE_LOST };
struct Command {
  enum CommandKind kind;
  int id, a, b, c;
  int64_t time;
  float x, y, vs, hs;
  struct MatonSurfaceOutput* output;
  struct wlr_buffer* buffer;
  int fence_fd;
  struct SocketSession* session;
  struct Command* next;
};
struct MatonSyncSurface {
  struct wl_resource* resource;
  struct wlr_surface* surface;
  struct wl_listener commit, destroy;
  struct wl_list link;
  int pending_fd, acquire_fd;
};
struct Window {
  int id, width, height, activity, session;
  bool stopped;
  struct MatonSizeHints hints;
  struct wl_listener size_hints, decorations;
  bool client_decorated;
  struct MatonSurfaceOutput surface;
  struct wlr_output* output;
  struct wlr_scene_output* scene_output;
  struct wlr_scene* scene;
  struct wlr_xdg_toplevel* toplevel;
  struct wlr_xwayland_surface* xsurface;
  struct wlr_scene_tree* scene_tree;
  struct MatonPresenter* presenter;
  struct wl_listener frame, commit, toplevel_destroy, surface_commit;
  /* A skipped shm frame is retried when a buffer is released or the client
   * commits another frame. Never poll for a free buffer: the headless output
   * can complete frames immediately and turn a failed allocation into an
   * unbounded SurfaceControl/Binder transaction stream. */
  bool present_pending;
  struct wl_listener xsurface_destroy, xsurface_associate, xsurface_geometry, scene_tree_destroy;
  /* Window-state requests (fullscreen, minimize) from X11 or xdg_toplevel. */
  struct wl_listener request_fullscreen, request_minimize, request_move, request_resize;
  struct Window* next;
};
struct Server {
  pthread_mutex_t mutex;
  pthread_cond_t ready_cond;
  pthread_cond_t xw_cond;
  pthread_t thread;
  atomic_bool started, stopping, demo_started;
  int event_fd, ready, ok, runtime_dir_fd, xwayland_dir_fd;
  char socket_name[108], runtime_dir[512];
  struct Command* first;
  struct Command* last;
  struct Window* windows;
  struct SocketSession* sessions;
  bool xw_done; unsigned xw_sent, xw_completed; bool xw_ok; char xw_display[128];
  struct wl_display* display;
  struct wl_list sync_surfaces;
  struct wl_global* sync_file_global;
  unsigned wsi_trace_count;
  struct wl_event_loop* loop;
  struct wlr_backend* backend;
  struct wlr_renderer* renderer;
  struct wlr_compositor* compositor;
  struct wlr_allocator* allocator;
  struct MatonAhbAllocator ahb_allocator;
  struct MatonEglUploader uploader;
  struct wlr_xdg_shell* xdg_shell;
  struct wlr_seat* seat;
  struct wlr_keyboard keyboard;
  int keyboard_initialized;
  struct wlr_pointer_constraints_v1* constraints;
  struct wlr_relative_pointer_manager_v1* relative;
  struct wlr_pointer_constraint_v1* constraint;
  struct wl_listener new_constraint, pointer_focus_change;
  double pointer_x, pointer_y;
  struct wlr_surface* pointer_focus;
  uint32_t pointer_buttons;
  int pointer_window;
  struct wl_listener new_toplevel, new_popup, new_decoration, new_client;
  char xwayland_path[512], xwayland_dir[512];
  struct XwaylandSession* xwayland_sessions;
  struct wlr_output* monitor;
  struct wlr_drm_format_set dmabuf_formats;
} server = { .mutex=PTHREAD_MUTEX_INITIALIZER, .ready_cond=PTHREAD_COND_INITIALIZER,
             .xw_cond=PTHREAD_COND_INITIALIZER,
             .event_fd=-1, .runtime_dir_fd=-1, .xwayland_dir_fd=-1 };

/* Minimal sync_file acquire protocol used by the freestanding Vulkan ICD. */
extern const struct wl_interface wl_surface_interface;
static const struct wl_interface maton_sync_surface_interface;
static const struct wl_interface *maton_sync_manager_types[]={&maton_sync_surface_interface,&wl_surface_interface};
static const struct wl_message maton_sync_manager_methods[]={
  {"destroy","1",NULL},{"get_surface_sync","no",maton_sync_manager_types},
};
static const struct wl_message maton_sync_surface_methods[]={
  {"destroy","1",NULL},{"set_acquire_fence","h",NULL},
};
static const struct wl_interface maton_sync_surface_interface={
  "maton_sync_surface_v1",1,2,maton_sync_surface_methods,0,NULL,
};
static const struct wl_interface maton_sync_file_interface={
  "maton_sync_file_v1",1,2,maton_sync_manager_methods,0,NULL,
};
static void destroy_sync_surface(struct wl_resource* resource);
static void sync_surface_commit(struct wl_listener* listener,void* data);
static void sync_surface_destroy(struct wl_listener* listener,void* data);
static void sync_surface_request_destroy(struct wl_client* client,struct wl_resource* resource) {(void)client;wl_resource_destroy(resource);}
static void sync_set_acquire_fence(struct wl_client* client,struct wl_resource* resource,int fd) {
  (void)client;struct MatonSyncSurface* sync=wl_resource_get_user_data(resource);
  if(sync->pending_fd>=0)close(sync->pending_fd);sync->pending_fd=fd;
}
static const struct { void (*destroy)(struct wl_client*,struct wl_resource*); void (*set_acquire_fence)(struct wl_client*,struct wl_resource*,int); } maton_sync_surface_impl={
  .destroy=sync_surface_request_destroy,
  .set_acquire_fence=sync_set_acquire_fence,
};
static void sync_manager_destroy(struct wl_client* client,struct wl_resource* resource) {(void)client;wl_resource_destroy(resource);}
static void sync_get_surface(struct wl_client* client,struct wl_resource* resource,uint32_t id,struct wl_resource* surface_resource) {
  (void)resource;struct wlr_surface* surface=wlr_surface_from_resource(surface_resource);
  if(!surface){wl_client_post_implementation_error(client,"sync_file requested for invalid wl_surface");return;}
  struct MatonSyncSurface* sync=calloc(1,sizeof(*sync));if(!sync){wl_client_post_no_memory(client);return;}
  sync->pending_fd=-1;sync->acquire_fd=-1;sync->surface=surface;
  sync->resource=wl_resource_create(client,&maton_sync_surface_interface,1,id);
  if(!sync->resource){free(sync);wl_client_post_no_memory(client);return;}
  wl_list_init(&sync->link);wl_list_insert(&server.sync_surfaces,&sync->link);
  sync->commit.notify=sync_surface_commit;wl_signal_add(&surface->events.commit,&sync->commit);
  sync->destroy.notify=sync_surface_destroy;wl_signal_add(&surface->events.destroy,&sync->destroy);
  wl_resource_set_implementation(sync->resource,&maton_sync_surface_impl,sync,destroy_sync_surface);
}
static const struct { void (*destroy)(struct wl_client*,struct wl_resource*); void (*get_surface_sync)(struct wl_client*,struct wl_resource*,uint32_t,struct wl_resource*); } maton_sync_manager_impl={
  .destroy=sync_manager_destroy,.get_surface_sync=sync_get_surface,
};
static void bind_sync_file(struct wl_client* client,void* data,uint32_t version,uint32_t id) {
  (void)data;struct wl_resource* resource=wl_resource_create(client,&maton_sync_file_interface,version>1?1:version,id);
  if(!resource){wl_client_post_no_memory(client);return;}
  wl_resource_set_implementation(resource,&maton_sync_manager_impl,NULL,NULL);
}
static void sync_surface_commit(struct wl_listener* listener,void* data) {
  (void)data;struct MatonSyncSurface* sync=(void*)((char*)listener - offsetof(struct MatonSyncSurface,commit));
  if(sync->pending_fd<0)return;
  if(sync->surface->buffer&&(sync->surface->current.committed&WLR_SURFACE_STATE_BUFFER)) {
    if(sync->acquire_fd>=0)close(sync->acquire_fd);sync->acquire_fd=sync->pending_fd;
  } else close(sync->pending_fd);
  sync->pending_fd=-1;
}
static void sync_surface_destroy(struct wl_listener* listener,void* data) {
  (void)data;struct MatonSyncSurface* sync=(void*)((char*)listener - offsetof(struct MatonSyncSurface,destroy));
  wl_resource_destroy(sync->resource);
}
static void destroy_sync_surface(struct wl_resource* resource) {
  struct MatonSyncSurface* sync=wl_resource_get_user_data(resource);if(!sync)return;
  if(sync->commit.link.prev){wl_list_remove(&sync->commit.link);wl_list_init(&sync->commit.link);}
  if(sync->destroy.link.prev){wl_list_remove(&sync->destroy.link);wl_list_init(&sync->destroy.link);}
  wl_list_remove(&sync->link);
  if(sync->pending_fd>=0)close(sync->pending_fd);if(sync->acquire_fd>=0)close(sync->acquire_fd);free(sync);
}
int maton_sync_take_acquire(struct wlr_surface* surface) {
  struct MatonSyncSurface* sync;
  wl_list_for_each(sync,&server.sync_surfaces,link)if(sync->surface==surface){int fd=sync->acquire_fd;sync->acquire_fd=-1;return fd;}
  return -1;
}

/* WindowActivity's built-in demo reserves ID 1. Real application windows
 * must never share it, including when the demo is opened after an app. */
static atomic_int next_window_id = 2;
static void handle_add_xwayland(int session,uid_t uid);
static void on_xsurface_commit(struct wl_listener* l,void* data);
static void window_state_unlisten(struct Window* w);
static struct wlr_surface* window_root_surface(struct Window* w);
static struct MatonFit window_fit(struct Window* w) {
  struct wlr_surface* surface=window_root_surface(w);
  int width=surface?surface->current.width:w->width,height=surface?surface->current.height:w->height;
  if(w->toplevel&&w->toplevel->base->geometry.width>0){width=w->toplevel->base->geometry.width;height=w->toplevel->base->geometry.height;}
  return maton_fit(w->width,w->height,width,height);
}
static void configure_window(struct Window* w) {
  /* Xwayland activates RandR viewport emulation only when the X drawable
   * matches the requested mode. Forcing a fullscreen 1280x720 drawable to
   * 1600x900 disables that path and leaves the GL viewport in one corner. */
  if(maton_size_fixed(w->hints)||(w->xsurface&&w->xsurface->fullscreen))return;
  int width=maton_size_clamp(w->width,w->hints.min_width,w->hints.max_width);
  int height=maton_size_clamp(w->height,w->hints.min_height,w->hints.max_height);
  if(w->toplevel&&w->toplevel->base->initialized)wlr_xdg_toplevel_set_size(w->toplevel,width,height);
  else if(w->xsurface&&(w->xsurface->width!=width||w->xsurface->height!=height||w->xsurface->x||w->xsurface->y))
    wlr_xwayland_surface_configure(w->xsurface,0,0,width,height);
}
struct PointerButtonEvent { uint32_t time; int window; };
static void notify_pointer_button(void* data, uint32_t code, bool pressed) {
  struct PointerButtonEvent* event = data;
  if(__android_log_is_loggable(ANDROID_LOG_DEBUG,"MatonPointer",ANDROID_LOG_INFO))
    __android_log_print(ANDROID_LOG_DEBUG,"MatonPointer","Window %d: button %u %s",event->window,code,pressed ? "pressed" : "released");
  wlr_seat_pointer_notify_button(server.seat,event->time,code,
      pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
}
/* All constraint state is owned by the Wayland thread. Deactivation can
 * destroy a one-shot constraint, so clear our reference before sending. */
static void deactivate_constraint(void) {
  struct wlr_pointer_constraint_v1* c=server.constraint;
  if(!c)return;
  server.constraint=NULL;
  maton_java_window_state(server.pointer_window,MATON_STATE_CAPTURE_OFF);
  if(c->type==WLR_POINTER_CONSTRAINT_V1_LOCKED&&c->current.cursor_hint.enabled){
    server.pointer_x=c->current.cursor_hint.x;server.pointer_y=c->current.cursor_hint.y;
    if(server.seat->pointer_state.focused_surface==c->surface)wlr_seat_pointer_notify_motion(server.seat,0,server.pointer_x,server.pointer_y);
  }
  wlr_pointer_constraint_v1_send_deactivated(c);
}
static void activate_constraint(void) {
  if(server.constraint||!server.constraints||!server.pointer_window)return;
  struct wlr_pointer_constraint_v1* c=wlr_pointer_constraints_v1_constraint_for_surface(
      server.constraints,server.seat->pointer_state.focused_surface,server.seat);
  if(!c||!isfinite(server.pointer_x)||!isfinite(server.pointer_y)||!pixman_region32_contains_point(&c->region,(int)floor(server.pointer_x),(int)floor(server.pointer_y),NULL))return;
  server.constraint=c;wlr_pointer_constraint_v1_send_activated(c);
  maton_java_window_state(server.pointer_window,MATON_STATE_CAPTURE_ON);
}
struct Constraint { struct wlr_pointer_constraint_v1* constraint; struct wl_listener destroy, region; };
static void on_constraint_destroy(struct wl_listener* l,void* data) {
  (void)data;struct Constraint* c=(struct Constraint*)((char*)l-offsetof(struct Constraint,destroy));
  if(server.constraint==c->constraint){
    struct wlr_pointer_constraint_v1* constraint=c->constraint;server.constraint=NULL;
    if(constraint->type==WLR_POINTER_CONSTRAINT_V1_LOCKED&&constraint->current.cursor_hint.enabled&&server.seat->pointer_state.focused_surface==constraint->surface){
      server.pointer_x=constraint->current.cursor_hint.x;server.pointer_y=constraint->current.cursor_hint.y;
      wlr_seat_pointer_notify_motion(server.seat,0,server.pointer_x,server.pointer_y);
    }
    maton_java_window_state(server.pointer_window,MATON_STATE_CAPTURE_OFF);
  }
  wl_list_remove(&c->destroy.link);wl_list_remove(&c->region.link);free(c);
}
static void on_constraint_region(struct wl_listener* l,void* data) {
  (void)l;(void)data;
  if(server.constraint&&!pixman_region32_contains_point(&server.constraint->region,(int)floor(server.pointer_x),(int)floor(server.pointer_y),NULL))deactivate_constraint();
  activate_constraint();
}
static void on_new_constraint(struct wl_listener* l,void* data) {
  (void)l;struct Constraint* c=calloc(1,sizeof(*c));if(!c)return;c->constraint=data;
  c->destroy.notify=on_constraint_destroy;wl_signal_add(&c->constraint->events.destroy,&c->destroy);
  c->region.notify=on_constraint_region;wl_signal_add(&c->constraint->events.set_region,&c->region);
  activate_constraint();
}
static void on_pointer_focus_change(struct wl_listener* l,void* data) {
  (void)l;(void)data;
  if(server.constraint&&server.constraint->surface!=server.seat->pointer_state.focused_surface)deactivate_constraint();
  activate_constraint();
}
static void clear_window_pointer(struct Window* w) {
  if(!w || !server.seat || server.pointer_window!=w->id)return;
  deactivate_constraint();
  struct timespec now={0};clock_gettime(CLOCK_MONOTONIC,&now);
  struct PointerButtonEvent event={.time=(uint32_t)(now.tv_sec*1000+now.tv_nsec/1000000),.window=w->id};
  maton_pointer_buttons_update(&server.pointer_buttons,0,notify_pointer_button,&event);
  wlr_seat_pointer_notify_clear_focus(server.seat);
  wlr_seat_pointer_notify_frame(server.seat);
  server.pointer_focus=NULL;server.pointer_window=0;
}
static struct Window* window_from_listener(struct wl_listener* l, size_t offset) {
  return (struct Window*)((char*)l - offset);
}
static struct Window* find_window(int id) {
  for (struct Window* w=server.windows; w; w=w->next) if (w->id==id) return w;
  return NULL;
}
static void enqueue(struct Command* c) {
  pthread_mutex_lock(&server.mutex);
  if (atomic_load(&server.stopping) && c->kind != CMD_STOP) {
    pthread_mutex_unlock(&server.mutex);
    /* A release with an unsignaled fence must stay locked even while the
     * compositor is stopping. Leaking the lock is safer than early reuse. */
    if (c->buffer && c->kind != CMD_RELEASE) wlr_buffer_unlock(c->buffer);
    if (c->kind == CMD_RELEASE && c->fence_fd >= 0) close(c->fence_fd);
    if (c->output) { maton_surface_output_finish(c->output); free(c->output); }
    if (c->session) { close(c->session->fd); unlink(c->session->path); free(c->session); }
    free(c); return;
  }
  c->next=NULL;
  if (server.last) server.last->next=c; else server.first=c;
  server.last=c;
  int fd=server.event_fd;
  pthread_mutex_unlock(&server.mutex);
  if (fd>=0) { uint64_t one=1; (void)write(fd,&one,sizeof(one)); }
}
static void command(enum CommandKind kind,int id,int a,int b,int c,int64_t time,
                    float x,float y,float vs,float hs,struct MatonSurfaceOutput* output,
                    struct wlr_buffer* buffer) {
  struct Command* cmd=calloc(1,sizeof(*cmd));
  if (!cmd) { if(buffer)wlr_buffer_unlock(buffer); if(output){maton_surface_output_finish(output);free(output);} return; }
  cmd->fence_fd=-1;
  cmd->kind=kind;cmd->id=id;cmd->a=a;cmd->b=b;cmd->c=c;cmd->time=time;
  cmd->x=x;cmd->y=y;cmd->vs=vs;cmd->hs=hs;cmd->output=output;cmd->buffer=buffer;enqueue(cmd);
}
struct ReleaseFence {
  struct wlr_buffer* buffer;
  struct wl_event_source* source;
};
static void schedule_pending_presentations(void) {
  for(struct Window* w=server.windows;w;w=w->next)
    if(w->present_pending&&w->output&&!w->stopped)wlr_output_schedule_frame(w->output);
}
static int on_release_fence(int fd,uint32_t mask,void* data) {
  struct ReleaseFence* release=data;
  if(!(mask&(WL_EVENT_READABLE|WL_EVENT_HANGUP|WL_EVENT_ERROR)))return 0;
  if(release->source)wl_event_source_remove(release->source);
  close(fd);
  wlr_buffer_unlock(release->buffer);
  schedule_pending_presentations();
  free(release);
  return 0;
}
static void release_buffer(struct wlr_buffer* buffer,int fence_fd) {
  struct Command* cmd=calloc(1,sizeof(*cmd));
  if(!cmd){if(fence_fd>=0)close(fence_fd);return;}
  cmd->kind=CMD_RELEASE;cmd->buffer=buffer;cmd->fence_fd=fence_fd;
  enqueue(cmd);
}
static void track_release(struct wlr_buffer* buffer,int fd) {
  if(fd<0){
    wlr_buffer_unlock(buffer);
    schedule_pending_presentations();
    return;
  }
  struct ReleaseFence* release=calloc(1,sizeof(*release));
  if(!release){close(fd);return;}
  release->buffer=buffer;
  release->source=wl_event_loop_add_fd(server.loop,fd,
      WL_EVENT_READABLE|WL_EVENT_HANGUP|WL_EVENT_ERROR,on_release_fence,release);
  if(!release->source){close(fd);free(release);}
}
static void on_commit(struct wl_listener* l,void* data) {
  struct Window* w=window_from_listener(l,offsetof(struct Window,commit));
  /* A stopped Android window is no longer latched by SurfaceFlinger. Do not
   * hand it new buffers; the client commit listener still completes its
   * frame callbacks so eglSwapBuffers can return and release its wl_buffer. */
  if(w->stopped)return;
  struct wlr_output_event_commit* event=data;
  struct wlr_buffer* b=event->state->buffer;
  if(!b)return;
  AHardwareBuffer* ahb=maton_ahb_from_wlr_buffer(b);
  if(!ahb)return;
  if(!maton_ahb_buffer_is_direct(b)&&!maton_ahb_buffer_upload_fallback(b,&server.uploader)){
    __android_log_print(ANDROID_LOG_WARN,"MatonCompositor","AHardwareBuffer upload failed");return;
  }
  (void)maton_surface_output_present(&w->surface,ahb,b,-1);
}
static void on_frame(struct wl_listener* l,void* data) {
  (void)data;
  struct Window* w=window_from_listener(l,offsetof(struct Window,frame));
  /* Windows are not composited: each visible buffer becomes its own Android
   * layer (presenter.c), so client dma-bufs reach SurfaceFlinger untouched. */
  if(!w->stopped && w->scene_output&&w->presenter){
    /* Retry failed shm copies only after a buffer becomes available (release
     * fence) or another client commit schedules a frame. A periodic timer
     * drives an unbounded transaction stream when allocation cannot recover. */
    w->present_pending=maton_presenter_present(w->presenter,w->scene,window_fit(w),w->width,w->height);
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);wlr_scene_output_send_frame_done(w->scene_output,&now);
  }
}
static void destroy_output(struct Window* w) {
  clear_window_pointer(w);
  if(w->frame.link.prev){wl_list_remove(&w->frame.link);wl_list_init(&w->frame.link);}
  if(w->commit.link.prev){wl_list_remove(&w->commit.link);wl_list_init(&w->commit.link);}
  if(w->presenter)maton_presenter_destroy(w->presenter);w->presenter=NULL;
  if(w->scene_output)wlr_scene_output_destroy(w->scene_output);
  if(w->output)wlr_output_destroy(w->output);
  maton_surface_output_finish(&w->surface);w->scene_output=NULL;w->output=NULL;
}
static void destroy_window(struct Window* w) {
  window_state_unlisten(w);
  if(w->surface_commit.link.prev){wl_list_remove(&w->surface_commit.link);wl_list_init(&w->surface_commit.link);}
  if(w->toplevel_destroy.link.prev){wl_list_remove(&w->toplevel_destroy.link);wl_list_init(&w->toplevel_destroy.link);}
  if(w->xsurface_destroy.link.prev){wl_list_remove(&w->xsurface_destroy.link);wl_list_init(&w->xsurface_destroy.link);}
  if(w->xsurface_associate.link.prev){wl_list_remove(&w->xsurface_associate.link);wl_list_init(&w->xsurface_associate.link);}
  if(w->xsurface_geometry.link.prev){wl_list_remove(&w->xsurface_geometry.link);wl_list_init(&w->xsurface_geometry.link);}
  if(w->size_hints.link.prev)wl_list_remove(&w->size_hints.link);
  destroy_output(w);
  if(w->scene)wlr_scene_node_destroy(&w->scene->tree.node);
  struct Window** p=&server.windows;while(*p&&*p!=w)p=&(*p)->next;if(*p)*p=w->next;
  free(w);
}
static void window_state_unlisten(struct Window* w) {
  if(w->request_move.link.prev){wl_list_remove(&w->request_move.link);wl_list_init(&w->request_move.link);}
  if(w->request_resize.link.prev){wl_list_remove(&w->request_resize.link);wl_list_init(&w->request_resize.link);}
  if(w->decorations.link.prev){wl_list_remove(&w->decorations.link);wl_list_init(&w->decorations.link);}
  if(w->request_fullscreen.link.prev){wl_list_remove(&w->request_fullscreen.link);wl_list_init(&w->request_fullscreen.link);}
  if(w->request_minimize.link.prev){wl_list_remove(&w->request_minimize.link);wl_list_init(&w->request_minimize.link);}
}
static void on_toplevel_destroy(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,toplevel_destroy));
  wl_list_remove(&l->link);wl_list_init(&l->link);
  window_state_unlisten(w);
  wl_list_remove(&w->surface_commit.link);wl_list_init(&w->surface_commit.link);
  destroy_output(w);w->toplevel=NULL;w->activity=0;maton_java_close_window(w->id);
}
/* Keyboard focus belongs to the window, never to the popup or subsurface
 * under the pointer: moving it there makes toolkits see a focus-out on the
 * toplevel and dismiss their open menus. */
static struct wlr_surface* window_root_surface(struct Window* w) {
  if(w->toplevel&&w->toplevel->base->surface)return w->toplevel->base->surface;
  if(w->xsurface&&w->xsurface->surface)return w->xsurface->surface;
  return NULL;
}
static void on_surface_commit(struct wl_listener* l,void* data) {
  (void)data;
  struct Window* w=window_from_listener(l,offsetof(struct Window,surface_commit));
  if(w->stopped&&w->scene_output){
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    wlr_scene_output_send_frame_done(w->scene_output,&now);
  }
  if(w->toplevel){
    struct wlr_xdg_toplevel_state* t=&w->toplevel->current;
    struct MatonSizeHints h={t->min_width,t->min_height,t->max_width,t->max_height};
    if(w->toplevel->base->initial_commit||memcmp(&h,&w->hints,sizeof(h))){w->hints=h;
      __android_log_print(ANDROID_LOG_DEBUG,"MatonCompositor","xdg window %d: min=%dx%d max=%dx%d",w->id,h.min_width,h.min_height,h.max_width,h.max_height);
      configure_window(w);
    }
  }
  if(w->output&&!w->stopped)wlr_output_schedule_frame(w->output);
  /* wlroots initializes xdg surfaces on their first surface commit. */
  if(w->toplevel&&w->toplevel->base->initial_commit){
    configure_window(w);
    /* Android frames and manages the window, so it is tiled on every edge:
     * toolkits then drop rounded corners, shadow margins and resize borders.
     * Firefox ignores tiled for its own frame, so maximized is sent as well;
     * the window size stays whatever Android gives it. */
    wlr_xdg_toplevel_set_tiled(w->toplevel,WLR_EDGE_TOP|WLR_EDGE_BOTTOM|WLR_EDGE_LEFT|WLR_EDGE_RIGHT);
    wlr_xdg_toplevel_set_maximized(w->toplevel,true);
  }
}
/* The app asks; Android decides and maton_core_set_fullscreen confirms. */
static void on_toplevel_request_fullscreen(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,request_fullscreen));
  if(w->toplevel)maton_java_window_state(w->id,w->toplevel->requested.fullscreen?MATON_STATE_FULLSCREEN_ON:MATON_STATE_FULLSCREEN_OFF);
}
static void on_toplevel_request_minimize(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,request_minimize));
  maton_java_window_state(w->id,MATON_STATE_MINIMIZE);
}
/* A client cannot initiate geometry without a live primary-button press in
 * its own window. Wayland additionally requires the seat's grab serial.
 * Java/bridge repeats ownership checks; this per-app parser is untrusted. */
static void request_geometry(struct Window* w,uint32_t edges) {
  if(!w||!server.seat||server.constraint||server.pointer_window!=w->id||
      !(server.pointer_buttons&1)||edges>15||(edges&3)==3||(edges&12)==12)return;
  maton_java_window_state(w->id,MATON_STATE_GEOMETRY_BASE+(int)edges);
}
static void on_toplevel_request_move(struct wl_listener* l,void* data) {
  struct Window* w=window_from_listener(l,offsetof(struct Window,request_move));
  struct wlr_xdg_toplevel_move_event* e=data;
  if(e&&e->seat&&e->seat->seat==server.seat&&
      wlr_seat_validate_pointer_grab_serial(server.seat,window_root_surface(w),e->serial))request_geometry(w,0);
}
static void on_toplevel_request_resize(struct wl_listener* l,void* data) {
  struct Window* w=window_from_listener(l,offsetof(struct Window,request_resize));
  struct wlr_xdg_toplevel_resize_event* e=data;
  if(e&&e->edges&&e->seat&&e->seat->seat==server.seat&&
      wlr_seat_validate_pointer_grab_serial(server.seat,window_root_surface(w),e->serial))request_geometry(w,e->edges);
}
static void on_xrequest_move(struct wl_listener* l,void* data) {
  (void)data;request_geometry(window_from_listener(l,offsetof(struct Window,request_move)),0);
}
static void on_xrequest_resize(struct wl_listener* l,void* data) {
  struct wlr_xwayland_resize_event* e=data;
  if(e&&e->edges)request_geometry(window_from_listener(l,offsetof(struct Window,request_resize)),e->edges);
}
static void on_new_toplevel(struct wl_listener* l,void* data) {
  (void)l;struct wlr_xdg_toplevel* t=data;struct Window* w=NULL;
  int session=0;
  struct wl_client* client=wl_resource_get_client(t->base->surface->resource);
  struct sockaddr_un address={0};socklen_t size=sizeof(address);
  if(getsockname(wl_client_get_fd(client),(struct sockaddr*)&address,&size)==0)
    for(struct SocketSession* s=server.sessions;s;s=s->next)
      if(!strcmp(address.sun_path,s->path)){session=s->id;break;}
  for(struct Window* q=server.windows;q;q=q->next)if(!q->toplevel&&q->session==session){w=q;break;}
  int request=0;
  if(!w){w=calloc(1,sizeof(*w));if(!w)return;w->id=atomic_fetch_add(&next_window_id,1);w->width=640;w->height=400;w->scene=wlr_scene_create();if(!w->scene){free(w);return;}w->next=server.windows;server.windows=w;}
  request=!w->activity;w->session=session;w->toplevel=t;w->toplevel_destroy.notify=on_toplevel_destroy;
  wl_signal_add(&t->events.destroy,&w->toplevel_destroy);
  struct wlr_scene_tree* tree=wlr_scene_xdg_surface_create(&w->scene->tree,t->base);
  t->base->data=tree;
  if (!tree) {
    wl_list_remove(&w->toplevel_destroy.link); wl_list_init(&w->toplevel_destroy.link);
    w->toplevel=NULL; return;
  }
  w->surface_commit.notify=on_surface_commit;
  wl_signal_add(&t->base->surface->events.commit,&w->surface_commit);
  window_state_unlisten(w);
  w->request_fullscreen.notify=on_toplevel_request_fullscreen;wl_signal_add(&t->events.request_fullscreen,&w->request_fullscreen);
  w->request_minimize.notify=on_toplevel_request_minimize;wl_signal_add(&t->events.request_minimize,&w->request_minimize);
  w->request_move.notify=on_toplevel_request_move;wl_signal_add(&t->events.request_move,&w->request_move);
  w->request_resize.notify=on_toplevel_request_resize;wl_signal_add(&t->events.request_resize,&w->request_resize);
  /* Wayland toolkits without xdg-decoration (GTK) follow the button layout we
   * set (gtk-decoration-layout=:), so Android's caption supplies the buttons. */
  w->client_decorated=false;
  if(request){w->activity=1;maton_java_request_window(session,w->id,w->width,w->height);}
}
struct Popup {
  struct wlr_xdg_popup* popup;
  struct wl_listener commit, reposition, destroy;
};
/* Each window is its own Android output sized to the window, so anything a
 * popup draws past the window edge is cut off. Popups are therefore
 * constrained to the window: the positioner rules flip or slide menus back
 * inside it, as a desktop compositor does at the screen edge. */
static void constrain_popup(struct wlr_xdg_popup* popup) {
  if(!popup->base->initialized)return;
  struct wlr_surface* surface=popup->parent;struct wlr_xdg_surface* xdg;
  while((xdg=wlr_xdg_surface_try_from_wlr_surface(surface))&&xdg->role==WLR_XDG_SURFACE_ROLE_POPUP&&xdg->popup)
    surface=xdg->popup->parent;
  for(struct Window* w=server.windows;w;w=w->next){
    if(!xdg||!w->toplevel||w->toplevel->base!=xdg)continue;
    /* The scene places the window geometry, not the surface, at the output origin. */
    struct MatonFit fit=window_fit(w);
    struct wlr_box box={xdg->geometry.x,xdg->geometry.y,fit.width,fit.height};
    wlr_xdg_popup_unconstrain_from_box(popup,&box);return;
  }
  wlr_xdg_surface_schedule_configure(popup->base);
}
static void on_popup_commit(struct wl_listener* l,void* data) {
  (void)data;struct Popup* p=(struct Popup*)((char*)l - offsetof(struct Popup,commit));
  if(p->popup->base->initial_commit)constrain_popup(p->popup);
}
static void on_popup_reposition(struct wl_listener* l,void* data) {
  (void)data;struct Popup* p=(struct Popup*)((char*)l - offsetof(struct Popup,reposition));
  constrain_popup(p->popup);
}
static void on_popup_destroy(struct wl_listener* l,void* data) {
  (void)data;struct Popup* p=(struct Popup*)((char*)l - offsetof(struct Popup,destroy));
  wl_list_remove(&p->commit.link);wl_list_remove(&p->reposition.link);wl_list_remove(&p->destroy.link);free(p);
}
static void on_new_popup(struct wl_listener* listener,void* data) {
  (void)listener;struct wlr_xdg_popup* popup=data;
  struct wlr_xdg_surface* parent=wlr_xdg_surface_try_from_wlr_surface(popup->parent);
  if(!parent||!parent->data)return;
  popup->base->data=wlr_scene_xdg_surface_create(parent->data,popup->base);
  if(!popup->base->data)return;
  struct Popup* p=calloc(1,sizeof(*p));if(!p)return;p->popup=popup;
  p->commit.notify=on_popup_commit;wl_signal_add(&popup->base->surface->events.commit,&p->commit);
  p->reposition.notify=on_popup_reposition;wl_signal_add(&popup->events.reposition,&p->reposition);
  p->destroy.notify=on_popup_destroy;wl_signal_add(&popup->events.destroy,&p->destroy);
}
/* Apps that support server-side decorations always get them: Android's
 * caption is the frame. Only X11 windows that turn off the WM title (Steam,
 * games) keep their own header and hide Android's caption. */
struct Decoration {
  struct wlr_xdg_toplevel_decoration_v1* deco;
  struct wl_listener request, commit, destroy;
};
/* wlroots asserts if a configure is scheduled before the surface's initial
 * commit, so the mode is sent then, or at once when already initialized. */
static void decoration_send(struct Decoration* d) {
  if(d->deco->toplevel->base->initialized)
    wlr_xdg_toplevel_decoration_v1_set_mode(d->deco,WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static void on_decoration_request(struct wl_listener* l,void* data) {
  (void)data;decoration_send((struct Decoration*)((char*)l-offsetof(struct Decoration,request)));
}
static void on_decoration_commit(struct wl_listener* l,void* data) {
  (void)data;struct Decoration* d=(struct Decoration*)((char*)l-offsetof(struct Decoration,commit));
  if(d->deco->toplevel->base->initial_commit)decoration_send(d);
  for(struct Window* w=server.windows;w;w=w->next)if(w->toplevel==d->deco->toplevel){w->client_decorated=d->deco->current.mode!=WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;maton_java_window_state(w->id,w->client_decorated?MATON_STATE_CLIENT_DECORATED:MATON_STATE_SERVER_DECORATED);}
}
static void on_decoration_destroy(struct wl_listener* l,void* data) {
  (void)data;struct Decoration* d=(struct Decoration*)((char*)l-offsetof(struct Decoration,destroy));
  for(struct Window* w=server.windows;w;w=w->next)if(w->toplevel==d->deco->toplevel){w->client_decorated=false;maton_java_window_state(w->id,MATON_STATE_SERVER_DECORATED);}
  wl_list_remove(&d->request.link);wl_list_remove(&d->commit.link);wl_list_remove(&d->destroy.link);free(d);
}
static void on_new_decoration(struct wl_listener* l,void* data) {
  (void)l;struct wlr_xdg_toplevel_decoration_v1* deco=data;
  struct Decoration* d=calloc(1,sizeof(*d));if(!d)return;
  d->deco=deco;
  d->request.notify=on_decoration_request;wl_signal_add(&deco->events.request_mode,&d->request);
  d->commit.notify=on_decoration_commit;wl_signal_add(&deco->toplevel->base->surface->events.commit,&d->commit);
  d->destroy.notify=on_decoration_destroy;wl_signal_add(&deco->events.destroy,&d->destroy);
  decoration_send(d);
}
/* Xwayland windows reuse the toplevel window path: one Android activity per
 * managed X window, rendered through a scene subsurface tree. */
static void on_xsurface_destroy(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,xsurface_destroy));
  wl_list_remove(&l->link);wl_list_init(&l->link);
  wl_list_remove(&w->surface_commit.link);wl_list_init(&w->surface_commit.link);
  if(w->xsurface_associate.link.prev){wl_list_remove(&w->xsurface_associate.link);wl_list_init(&w->xsurface_associate.link);}
  if(w->xsurface_geometry.link.prev){wl_list_remove(&w->xsurface_geometry.link);wl_list_init(&w->xsurface_geometry.link);}
  if(w->size_hints.link.prev){wl_list_remove(&w->size_hints.link);wl_list_init(&w->size_hints.link);}
  window_state_unlisten(w);
  clear_window_pointer(w);destroy_output(w);w->xsurface=NULL;w->activity=0;
  maton_java_close_window(w->id);
}
/* The X window manager only lets a client move focus (e.g. to its menu)
 * between windows of the app it last activated; with none activated every
 * change is reverted and toolkits close their menus on the focus loss. */
static void xwindow_activate(struct Window* w) {
  if(w&&w->xsurface)wlr_xwayland_surface_activate(w->xsurface,true);
}
/* wlroots destroys the tree itself when the surface goes (client exit). */
static void on_scene_tree_destroy(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,scene_tree_destroy));
  wl_list_remove(&l->link);wl_list_init(&l->link);
  w->scene_tree=NULL;
}
static void xwindow_show_surface(struct Window* w) {
  if(!w->xsurface||!w->xsurface->surface)return;
  /* The scene renders the paired Wayland surface; Android owns presentation. */
  if(!w->scene_tree&&w->scene){
    w->scene_tree=wlr_scene_subsurface_tree_create(&w->scene->tree,w->xsurface->surface);
    if(w->scene_tree){w->scene_tree_destroy.notify=on_scene_tree_destroy;wl_signal_add(&w->scene_tree->node.events.destroy,&w->scene_tree_destroy);}
  }
  /* X11 has no tiled state; maximized makes toolkits drop frame effects. */
  wlr_xwayland_surface_set_maximized(w->xsurface,true,true);
  xwindow_activate(w);
  /* Resizable windows use the X origin; fixed windows retain their geometry. */
  configure_window(w);
}
static void on_xsurface_geometry(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,xsurface_geometry));
  /* Apply Android size within the current constraints. */
  if(w->xsurface)
    configure_window(w);
}
/* wlroots updates xs->fullscreen from _NET_WM_STATE before emitting. */
static void on_xsurface_request_fullscreen(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,request_fullscreen));
  if(w->xsurface)maton_java_window_state(w->id,w->xsurface->fullscreen?MATON_STATE_FULLSCREEN_ON:MATON_STATE_FULLSCREEN_OFF);
}
static void on_xsurface_request_minimize(struct wl_listener* l,void* data) {
  struct Window* w=window_from_listener(l,offsetof(struct Window,request_minimize));
  struct wlr_xwayland_minimize_event* e=data;
  if(e&&e->minimize)maton_java_window_state(w->id,MATON_STATE_MINIMIZE);
}
static void read_xsize_hints(struct Window* w) {
  xcb_size_hints_t* h=w->xsurface->size_hints;
  w->hints=(struct MatonSizeHints){0};
  if(h){
    if(h->flags&XCB_ICCCM_SIZE_HINT_P_MIN_SIZE){w->hints.min_width=h->min_width>0?h->min_width:0;w->hints.min_height=h->min_height>0?h->min_height:0;}
    if(h->flags&XCB_ICCCM_SIZE_HINT_P_MAX_SIZE){w->hints.max_width=h->max_width>0?h->max_width:0;w->hints.max_height=h->max_height>0?h->max_height:0;}
  }
}
static void on_xsize_hints(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,size_hints));
  read_xsize_hints(w);configure_window(w);
  __android_log_print(ANDROID_LOG_DEBUG,"MatonCompositor","X window %d: min=%dx%d max=%dx%d",w->id,w->hints.min_width,w->hints.min_height,w->hints.max_width,w->hints.max_height);
}
static void on_xdecorations(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,decorations));
  w->client_decorated=(w->xsurface->decorations&WLR_XWAYLAND_SURFACE_DECORATIONS_NO_TITLE)!=0;
  maton_java_window_state(w->id,w->client_decorated?MATON_STATE_CLIENT_DECORATED:MATON_STATE_SERVER_DECORATED);
}
static void close_xwindow(struct Window* w) {
  xcb_atom_t atom=XCB_ATOM_NONE;
  for(struct XwaylandSession* xw=server.xwayland_sessions;xw;xw=xw->next)if(xw->session==w->session)atom=xw->delete_window;
  for(size_t i=0;atom&&i<w->xsurface->protocols_len;i++)if(w->xsurface->protocols[i]==atom){wlr_xwayland_surface_close(w->xsurface);return;}
  __android_log_print(ANDROID_LOG_INFO,"MatonCompositor","X window %d class=%s: close without WM_DELETE_WINDOW; leaving client alive",w->id,w->xsurface->class?w->xsurface->class:"");
}
static struct Window* xwayland_window(struct wlr_xwayland_surface* xs,int session) {
  for(struct Window* w=server.windows;w;w=w->next)if(w->xsurface==xs)return w;
  struct Window* w=calloc(1,sizeof(*w));if(!w)return NULL;
  w->id=atomic_fetch_add(&next_window_id,1);
  w->width=xs->width>0?(int)xs->width:640;w->height=xs->height>0?(int)xs->height:400;
  w->scene=wlr_scene_create();
  if(!w->scene){free(w);return NULL;}
  w->next=server.windows;server.windows=w;
  w->session=session;w->xsurface=xs;
  w->request_move.notify=on_xrequest_move;wl_signal_add(&xs->events.request_move,&w->request_move);
  w->request_resize.notify=on_xrequest_resize;wl_signal_add(&xs->events.request_resize,&w->request_resize);
  w->decorations.notify=on_xdecorations;wl_signal_add(&xs->events.set_decorations,&w->decorations);
  w->client_decorated=(xs->decorations&WLR_XWAYLAND_SURFACE_DECORATIONS_NO_TITLE)!=0;
  read_xsize_hints(w);w->size_hints.notify=on_xsize_hints;wl_signal_add(&xs->events.set_size_hints,&w->size_hints);
  w->xsurface_destroy.notify=on_xsurface_destroy;
  wl_signal_add(&xs->events.destroy,&w->xsurface_destroy);
  w->xsurface_geometry.notify=on_xsurface_geometry;
  wl_signal_add(&xs->events.set_geometry,&w->xsurface_geometry);
  w->surface_commit.notify=on_xsurface_commit;
  wl_signal_add(&xs->events.request_configure,&w->surface_commit);
  w->request_fullscreen.notify=on_xsurface_request_fullscreen;wl_signal_add(&xs->events.request_fullscreen,&w->request_fullscreen);
  w->request_minimize.notify=on_xsurface_request_minimize;wl_signal_add(&xs->events.request_minimize,&w->request_minimize);
  __android_log_print(ANDROID_LOG_DEBUG,"MatonCompositor",
      "X window %d: title=\"%s\" class=%s %ux%u%+d%+d parent=%s modal=%d min=%d withdrawn=%d skip_taskbar=%d skip_pager=%d min_size=%dx%d max_size=%dx%d",
      w->id,xs->title?xs->title:"",xs->class?xs->class:"",xs->width,xs->height,xs->x,xs->y,
      xs->parent?"yes":"no",xs->modal,xs->minimized,xs->withdrawn,xs->skip_taskbar,xs->skip_pager,w->hints.min_width,w->hints.min_height,w->hints.max_width,w->hints.max_height);
  __android_log_print(ANDROID_LOG_DEBUG,"MatonCompositor","X window %d: WM_PROTOCOLS count=%zu",w->id,xs->protocols_len);
  for(size_t i=0;i<xs->protocols_len;i++)__android_log_print(ANDROID_LOG_DEBUG,"MatonCompositor","X window %d: WM_PROTOCOLS[%zu]=%u",w->id,i,xs->protocols[i]);
  w->activity=1;maton_java_request_window(session,w->id,w->width,w->height);
  /* Windows mapped already fullscreen (e.g. Steam Big Picture) ask now. */
  if(xs->fullscreen)maton_java_window_state(w->id,MATON_STATE_FULLSCREEN_ON);
  xwindow_show_surface(w);
  return w;
}
static void on_xsurface_commit(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,surface_commit));
  if(w->xsurface)configure_window(w);
}
/* Override-redirect windows (menus, tooltips, drop-downs) get no Android
 * window of their own. Root coordinates are translated relative to the owner
 * (fixed windows keep their requested X position):
 * the transient parent if set, else the window under the pointer, else the
 * session's newest X window. */
struct XOverlay {
  struct wlr_xwayland_surface* xs;
  struct wlr_scene_tree* tree;
  int session;
  struct wl_listener associate, dissociate, geometry, destroy, tree_destroy;
};
static struct Window* xoverlay_owner(struct XOverlay* o) {
  for(struct wlr_xwayland_surface* p=o->xs->parent;p;p=p->parent)
    for(struct Window* w=server.windows;w;w=w->next)if(w->xsurface==p)return w;
  struct Window* newest=NULL;
  for(struct Window* w=server.windows;w;w=w->next){
    if(!w->xsurface||w->session!=o->session||!w->scene)continue;
    if(w->id==server.pointer_window)return w;
    if(!newest||w->id>newest->id)newest=w;
  }
  return newest;
}
/* The tree can also die with its owner window's scene; forget it then. */
static void on_xoverlay_tree_destroy(struct wl_listener* l,void* data) {
  (void)data;struct XOverlay* o=(struct XOverlay*)((char*)l-offsetof(struct XOverlay,tree_destroy));
  wl_list_remove(&o->tree_destroy.link);wl_list_init(&o->tree_destroy.link);
  o->tree=NULL;
}
static void xoverlay_remove_tree(struct XOverlay* o) {
  if(o->tree)wlr_scene_node_destroy(&o->tree->node);
}
static void on_xoverlay_associate(struct wl_listener* l,void* data) {
  (void)data;struct XOverlay* o=(struct XOverlay*)((char*)l-offsetof(struct XOverlay,associate));
  struct Window* w=xoverlay_owner(o);
  if(!w||!o->xs->surface)return;
  xoverlay_remove_tree(o);
  o->tree=wlr_scene_subsurface_tree_create(&w->scene->tree,o->xs->surface);
  if(!o->tree)return;
  o->tree_destroy.notify=on_xoverlay_tree_destroy;wl_signal_add(&o->tree->node.events.destroy,&o->tree_destroy);
  wlr_scene_node_raise_to_top(&o->tree->node);
  wlr_scene_node_set_position(&o->tree->node,o->xs->x-w->xsurface->x,o->xs->y-w->xsurface->y);
  __android_log_print(ANDROID_LOG_DEBUG,"MatonCompositor","X overlay %ux%u+%d+%d class=%s on window %d",
      o->xs->width,o->xs->height,o->xs->x,o->xs->y,o->xs->class?o->xs->class:"-",w->id);
  if(w->output && !w->stopped)wlr_output_schedule_frame(w->output);
}
static void on_xoverlay_dissociate(struct wl_listener* l,void* data) {
  (void)data;struct XOverlay* o=(struct XOverlay*)((char*)l-offsetof(struct XOverlay,dissociate));
  xoverlay_remove_tree(o);
}
static void on_xoverlay_geometry(struct wl_listener* l,void* data) {
  (void)data;struct XOverlay* o=(struct XOverlay*)((char*)l-offsetof(struct XOverlay,geometry));
  struct Window* w=xoverlay_owner(o);
  if(o->tree&&w)wlr_scene_node_set_position(&o->tree->node,o->xs->x-w->xsurface->x,o->xs->y-w->xsurface->y);
}
static void on_xoverlay_destroy(struct wl_listener* l,void* data) {
  (void)data;struct XOverlay* o=(struct XOverlay*)((char*)l-offsetof(struct XOverlay,destroy));
  xoverlay_remove_tree(o);
  wl_list_remove(&o->associate.link);wl_list_remove(&o->dissociate.link);
  wl_list_remove(&o->geometry.link);wl_list_remove(&o->destroy.link);
  free(o);
}
static void xwayland_overlay(struct wlr_xwayland_surface* xs,int session) {
  struct XOverlay* o=calloc(1,sizeof(*o));if(!o)return;
  o->xs=xs;o->session=session;wl_list_init(&o->tree_destroy.link);
  o->associate.notify=on_xoverlay_associate;wl_signal_add(&xs->events.associate,&o->associate);
  o->dissociate.notify=on_xoverlay_dissociate;wl_signal_add(&xs->events.dissociate,&o->dissociate);
  o->geometry.notify=on_xoverlay_geometry;wl_signal_add(&xs->events.set_geometry,&o->geometry);
  o->destroy.notify=on_xoverlay_destroy;wl_signal_add(&xs->events.destroy,&o->destroy);
}
/* Toolkits create many managed X windows that are never shown (Chromium's
 * helper and InputOnly windows). An Android window exists only while the X
 * window is mapped; until then its own configure requests are honored. */
struct XTrack {
  struct wlr_xwayland_surface* xs;
  int session;
  struct wl_listener associate, dissociate, configure, destroy;
};
static struct Window* xtrack_window(struct XTrack* t) {
  for(struct Window* w=server.windows;w;w=w->next)if(w->xsurface==t->xs)return w;
  return NULL;
}
static void on_xtrack_associate(struct wl_listener* l,void* data) {
  (void)data;struct XTrack* t=(struct XTrack*)((char*)l-offsetof(struct XTrack,associate));
  if(!t->xs->override_redirect&&!xtrack_window(t))xwayland_window(t->xs,t->session);
}
static void on_xtrack_dissociate(struct wl_listener* l,void* data) {
  (void)data;struct XTrack* t=(struct XTrack*)((char*)l-offsetof(struct XTrack,dissociate));
  struct Window* w=xtrack_window(t);
  if(!w)return;
  if(w->scene_tree){wlr_scene_node_destroy(&w->scene_tree->node);w->scene_tree=NULL;}
  on_xsurface_destroy(&w->xsurface_destroy,NULL);
}
static void on_xtrack_configure(struct wl_listener* l,void* data) {
  struct XTrack* t=(struct XTrack*)((char*)l-offsetof(struct XTrack,configure));
  struct wlr_xwayland_surface_configure_event* ev=data;
  struct Window* w=xtrack_window(t);
  if(!w||t->xs->fullscreen)wlr_xwayland_surface_configure(t->xs,ev->x,ev->y,ev->width,ev->height);
  else configure_window(w);
}
static void on_xtrack_destroy(struct wl_listener* l,void* data) {
  (void)data;struct XTrack* t=(struct XTrack*)((char*)l-offsetof(struct XTrack,destroy));
  wl_list_remove(&t->associate.link);wl_list_remove(&t->dissociate.link);
  wl_list_remove(&t->configure.link);wl_list_remove(&t->destroy.link);
  free(t);
}
static void xwayland_track(struct wlr_xwayland_surface* xs,int session) {
  struct XTrack* t=calloc(1,sizeof(*t));if(!t)return;
  t->xs=xs;t->session=session;
  t->associate.notify=on_xtrack_associate;wl_signal_add(&xs->events.associate,&t->associate);
  t->dissociate.notify=on_xtrack_dissociate;wl_signal_add(&xs->events.dissociate,&t->dissociate);
  t->configure.notify=on_xtrack_configure;wl_signal_add(&xs->events.request_configure,&t->configure);
  t->destroy.notify=on_xtrack_destroy;wl_signal_add(&xs->events.destroy,&t->destroy);
}
static void on_xwayland_new_surface(struct wl_listener* l,void* data) {
  struct XwaylandSession* xw=(struct XwaylandSession*)((char*)l-offsetof(struct XwaylandSession,new_surface));
  struct wlr_xwayland_surface* xs=data;
  if(xs->override_redirect)xwayland_overlay(xs,xw->session);
  else xwayland_track(xs,xw->session);
}
static void xwayland_apply_socket_mode(struct XwaylandSession* xw) {
  if(!xw->xwayland)return;
  const char* dir=getenv("WLR_XWAYLAND_SOCKET_DIR");
  if(!dir||!dir[0])return;
  char path[256];snprintf(path,sizeof(path),"%s/X%s",dir,xw->xwayland->display_name+1);
  struct stat info;
  if(stat(path,&info)==0){
    /* Per-app Xwayland and its clients share a UID. */
    (void)chmod(path,info.st_uid==xw->uid?0600:0666);
  }
}
static void on_xwayland_ready(struct wl_listener* l,void* data) {
  (void)data;struct XwaylandSession* xw=(struct XwaylandSession*)((char*)l-offsetof(struct XwaylandSession,ready));
  __android_log_print(ANDROID_LOG_INFO,"MatonCompositor","Session %d: Xwayland ready on %s",
      xw->session,xw->xwayland?xw->xwayland->display_name:"?");
  if(xw->process_fd>=0)maton_xwayland_end(xw->process_fd,xw->process_pid);
  char binary[512];snprintf(binary,sizeof(binary),"%s/libmatonos-Xwayland-server.so",server.xwayland_path);
  xw->process_fd=maton_xwayland_pin(binary,&xw->process_pid);
  xwayland_apply_socket_mode(xw);
  /* Xwayland uses our private socket directory, not /tmp/.X11-unix. */
  int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
  struct sockaddr_un address={.sun_family=AF_UNIX};
  int length=snprintf(address.sun_path,sizeof(address.sun_path),"%s/X%s",server.xwayland_dir,xw->xwayland->display_name+1);
  xcb_connection_t* connection=NULL;
  if(fd>=0){
    if(length>0&&length<(int)sizeof(address.sun_path)&&!connect(fd,(struct sockaddr*)&address,sizeof(address)))connection=xcb_connect_to_fd(fd,NULL);
    else close(fd);
  }
  if(connection&&!xcb_connection_has_error(connection)){
    const char name[]="WM_DELETE_WINDOW";
    xcb_intern_atom_reply_t* reply=xcb_intern_atom_reply(connection,xcb_intern_atom(connection,0,sizeof(name)-1,name),NULL);
    if(reply){xw->delete_window=reply->atom;free(reply);}
  }
  if(connection)xcb_disconnect(connection);
  if(!xw->delete_window)__android_log_print(ANDROID_LOG_WARN,"MatonCompositor","Session %d: cannot intern WM_DELETE_WINDOW; X close requests leave clients alive",xw->session);
}
static void on_xwayland_destroy(struct wl_listener* l,void* data) {
  (void)data;struct XwaylandSession* xw=(struct XwaylandSession*)((char*)l-offsetof(struct XwaylandSession,destroy));
  if(xw->process_fd<0){
    char binary[512];snprintf(binary,sizeof(binary),"%s/libmatonos-Xwayland-server.so",server.xwayland_path);
    xw->process_fd=maton_xwayland_pin(binary,&xw->process_pid);
  }
  maton_xwayland_end(xw->process_fd,xw->process_pid);
  wl_list_remove(&xw->ready.link);wl_list_remove(&xw->new_surface.link);wl_list_remove(&xw->destroy.link);
  struct XwaylandSession** p=&server.xwayland_sessions;
  while(*p&&*p!=xw)p=&(*p)->next;if(*p)*p=xw->next;
  free(xw);
}
/* The single advertised monitor is what clients take as the screen: GTK and
 * Firefox place and track popups within it, and it is Xwayland's root window,
 * to which the X pointer is clamped. It starts at the display's size (sent
 * from Java as a resize of window 0) and grows to cover any larger Android
 * window; it never shrinks, which would push content past the root edge. */
static void monitor_cover(int width,int height) {
  struct wlr_output* m=server.monitor;
  if(!m||(width<=m->width&&height<=m->height))return;
  int w=width>m->width?width:m->width,h=height>m->height?height:m->height;
  struct wlr_output_state state;wlr_output_state_init(&state);
  wlr_output_state_set_custom_mode(&state,w,h,60000);
  if(!wlr_output_commit_state(m,&state))
    __android_log_print(ANDROID_LOG_ERROR,"MatonCompositor","Monitor resize to %dx%d failed",w,h);
  wlr_output_state_finish(&state);
}
static void attach_output(struct Command* c) {
  struct Window* w=find_window(c->id);
  if(!c->output)return;
  if(!w){w=calloc(1,sizeof(*w));if(!w){maton_surface_output_finish(c->output);free(c->output);return;}w->id=c->id;w->scene=wlr_scene_create();if(!w->scene){free(w);maton_surface_output_finish(c->output);free(c->output);return;}w->next=server.windows;server.windows=w;}
  if(w->output)destroy_output(w);
  w->width=c->a>0?c->a:(w->width?w->width:640);w->height=c->b>0?c->b:(w->height?w->height:400);
  w->surface=*c->output;free(c->output);w->activity=1;
  w->output=wlr_headless_add_output(server.backend,w->width,w->height);
  if(!w->output||!wlr_output_init_render(w->output,server.allocator,server.renderer)){
    __android_log_print(ANDROID_LOG_ERROR,"MatonCompositor","Window %d: output renderer initialization failed",w->id);
    destroy_output(w);return;
  }
  char name[48];snprintf(name,sizeof(name),"maton-window-%d",w->id);wlr_output_set_name(w->output,name);
  /* Android presentation needs our AHardwareBuffer, not a Wayland client's
   * SHM buffer submitted directly by the scene's scanout optimization. */
  wlr_output_lock_attach_render(w->output,true);
  struct wlr_output_state state;wlr_output_state_init(&state);wlr_output_state_set_enabled(&state,true);
  /* The AHardwareBuffer allocator supplies BGRA with alpha. wlroots defaults
   * to XRGB, which this allocator cannot create even for the initial clear. */
  wlr_output_state_set_render_format(&state,DRM_FORMAT_ARGB8888);
  wlr_output_state_set_custom_mode(&state,w->width,w->height,60000);
  bool ok=wlr_output_commit_state(w->output,&state);wlr_output_state_finish(&state);if(!ok){
    __android_log_print(ANDROID_LOG_ERROR,"MatonCompositor","Window %d: initial ARGB output commit failed (%dx%d)",w->id,w->width,w->height);
    destroy_output(w);return;
  }
  __android_log_print(ANDROID_LOG_INFO,"MatonCompositor","Window %d: ARGB output ready (%dx%d)",w->id,w->width,w->height);
  w->scene_output=wlr_scene_output_create(w->scene,w->output);if(!w->scene_output){destroy_output(w);return;}
  w->presenter=maton_presenter_create(w->surface.control,server.allocator,&server.uploader);
  if(!w->presenter){destroy_output(w);return;}
  configure_window(w);
  w->frame.notify=on_frame;wl_signal_add(&w->output->events.frame,&w->frame);
  w->commit.notify=on_commit;wl_signal_add(&w->output->events.commit,&w->commit);wlr_output_schedule_frame(w->output);
}
static void process_commands(void) {
  uint64_t value;while(read(server.event_fd,&value,sizeof(value))>0){}
  pthread_mutex_lock(&server.mutex);struct Command* list=server.first;server.first=server.last=NULL;pthread_mutex_unlock(&server.mutex);
  while(list){struct Command* c=list;list=c->next;struct Window* w=find_window(c->id);
    switch(c->kind){
    case CMD_ADD_SESSION:
      if(wl_display_add_socket_fd(server.display,c->session->fd)==0){
        c->session->next=server.sessions;server.sessions=c->session;
      }else{
        __android_log_print(ANDROID_LOG_ERROR,"MatonCompositor","Session %d: cannot register Wayland socket",c->session->id);
        close(c->session->fd);unlink(c->session->path);free(c->session);
      }
      break;
    case CMD_ADD_XWAYLAND:handle_add_xwayland(c->a,(uid_t)c->b);break;
    case CMD_STOPPED:
      if(w){w->stopped=c->a;
        if(w->stopped)clear_window_pointer(w);
        if(w->toplevel && w->toplevel->base->initialized)wlr_xdg_toplevel_set_suspended(w->toplevel,w->stopped);
        if(!w->stopped && w->output)wlr_output_schedule_frame(w->output);
      }break;
    case CMD_ATTACH:if(w)maton_java_window_state(w->id,w->client_decorated?MATON_STATE_CLIENT_DECORATED:MATON_STATE_SERVER_DECORATED);monitor_cover(c->a,c->b);attach_output(c);break;
    case CMD_DETACH:if(w){w->activity=0;destroy_output(w);}break;
    case CMD_RESIZE:if(c->a>0&&c->b>0)monitor_cover(c->a,c->b);if(w&&c->a>0&&c->b>0){w->width=c->a;w->height=c->b;if(w->output){struct wlr_output_state s;wlr_output_state_init(&s);wlr_output_state_set_custom_mode(&s,c->a,c->b,60000);bool committed=wlr_output_commit_state(w->output,&s);wlr_output_state_finish(&s);if(committed){configure_window(w);wlr_output_schedule_frame(w->output);}}}break;
    case CMD_KEY:if(server.seat&&server.keyboard_initialized){
      /* Java getScanCode() is raw evdev; preserve the positive-scan path.
       * Synthetic keys have no scan code and need Generic.kl's inverse. */
      int keycode=c->b>0?c->b:maton_android_keycode_to_evdev(c->a);
      if(keycode<0){
        static bool logged_unknown_key=false;
        if(!logged_unknown_key){wlr_log(WLR_DEBUG,"Dropping unmapped Android keycode %d (scan %d)",c->a,c->b);logged_unknown_key=true;}
        break;
      }
      struct wlr_surface* focus=NULL;if(w&&w->toplevel&&w->toplevel->base->surface)focus=w->toplevel->base->surface;else if(w&&w->xsurface&&w->xsurface->surface)focus=w->xsurface->surface;if(focus&&server.seat->keyboard_state.focused_surface!=focus){wlr_seat_keyboard_notify_enter(server.seat,focus,NULL,0,&server.keyboard.modifiers);xwindow_activate(w);}struct wlr_keyboard_key_event e={.time_msec=(uint32_t)(c->time/1000000),.keycode=(uint32_t)keycode,.update_state=true,.state=c->c==0?WL_KEYBOARD_KEY_STATE_PRESSED:WL_KEYBOARD_KEY_STATE_RELEASED};wlr_keyboard_notify_key(&server.keyboard,&e);wlr_seat_keyboard_notify_key(server.seat,e.time_msec,e.keycode,e.state);wlr_seat_keyboard_notify_modifiers(server.seat,&server.keyboard.modifiers);}break;
    case CMD_CAPTURE_LOST:if(w)clear_window_pointer(w);break;
    case CMD_RELATIVE:if(w&&server.constraint&&server.pointer_window==w->id&&isfinite(c->x)&&isfinite(c->y)){
      double dx=c->x/window_fit(w).scale,dy=c->y/window_fit(w).scale;
      wlr_relative_pointer_manager_v1_send_relative_motion(server.relative,server.seat,c->time/1000,dx,dy,dx,dy);
      if(server.constraint->type==WLR_POINTER_CONSTRAINT_V1_CONFINED){
        double x,y;if(wlr_region_confine(&server.constraint->region,server.pointer_x,server.pointer_y,server.pointer_x+dx,server.pointer_y+dy,&x,&y)){
          server.pointer_x=x;server.pointer_y=y;wlr_seat_pointer_notify_motion(server.seat,c->time/1000000,x,y);
        }
      }
      struct PointerButtonEvent e={.time=c->time/1000000,.window=w->id};
      maton_pointer_buttons_update(&server.pointer_buttons,c->b,notify_pointer_button,&e);
      if(c->vs)wlr_seat_pointer_notify_axis(server.seat,e.time,WL_POINTER_AXIS_VERTICAL_SCROLL,-c->vs*15,(int32_t)lroundf(-c->vs*120),WL_POINTER_AXIS_SOURCE_WHEEL,WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
      if(c->hs)wlr_seat_pointer_notify_axis(server.seat,e.time,WL_POINTER_AXIS_HORIZONTAL_SCROLL,-c->hs*15,(int32_t)lroundf(-c->hs*120),WL_POINTER_AXIS_SOURCE_WHEEL,WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
      wlr_seat_pointer_notify_frame(server.seat);
    }break;
    case CMD_MOTION:if(server.seat&&w){if(server.constraint)break;if(c->a==10||c->a==3){clear_window_pointer(w);break;}uint32_t tm=(uint32_t)(c->time/1000000);double sx=0,sy=0;struct wlr_scene* scene=w?w->scene:NULL;double x,y;bool inside=maton_fit_input(window_fit(w),c->x,c->y,&x,&y);struct wlr_scene_node* node=scene&&inside?wlr_scene_node_at(&scene->tree.node,x,y,&sx,&sy):NULL;struct wlr_scene_buffer* sb=node&&node->type==WLR_SCENE_NODE_BUFFER?wlr_scene_buffer_from_node(node):NULL;struct wlr_scene_surface* ss=sb?wlr_scene_surface_try_from_buffer(sb):NULL;if(ss){if(server.pointer_window!=w->id){struct Window* previous=find_window(server.pointer_window);clear_window_pointer(previous);}
      server.pointer_x=sx;server.pointer_y=sy;server.pointer_window=w->id;/* Compare with the seat's real focus, not a cache: a destroyed surface
       * or a popup grab can clear or move it, and coordinates for the surface
       * under the pointer would then reach another one. */
      if(server.seat->pointer_state.focused_surface!=ss->surface){server.pointer_focus=ss->surface;wlr_seat_pointer_notify_enter(server.seat,ss->surface,sx,sy);struct wlr_surface* root=window_root_surface(w);if(root&&server.seat->keyboard_state.focused_surface!=root){wlr_seat_keyboard_notify_enter(server.seat,root,NULL,0,&server.keyboard.modifiers);xwindow_activate(w);}}wlr_seat_pointer_notify_motion(server.seat,tm,sx,sy);activate_constraint();}else{clear_window_pointer(w);server.pointer_focus=NULL;wlr_seat_pointer_notify_clear_focus(server.seat);}struct PointerButtonEvent button_event={.time=tm,.window=w->id};maton_pointer_buttons_update(&server.pointer_buttons,ss ? (uint32_t)c->b : 0,notify_pointer_button,&button_event);/* Android reports wheel notches as +-1.0 (fractions on touchpads); the
       * discrete argument is value120, 120 per notch. */
      if(ss&&c->vs)wlr_seat_pointer_notify_axis(server.seat,tm,WL_POINTER_AXIS_VERTICAL_SCROLL,-c->vs*15.0,(int32_t)lroundf(-c->vs*120.0f),WL_POINTER_AXIS_SOURCE_WHEEL,WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);if(ss&&c->hs)wlr_seat_pointer_notify_axis(server.seat,tm,WL_POINTER_AXIS_HORIZONTAL_SCROLL,-c->hs*15.0,(int32_t)lroundf(-c->hs*120.0f),WL_POINTER_AXIS_SOURCE_WHEEL,WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);wlr_seat_pointer_notify_frame(server.seat);}break;
    case CMD_RELEASE:if(c->buffer)track_release(c->buffer,c->fence_fd);else if(c->fence_fd>=0)close(c->fence_fd);break;
    case CMD_FULLSCREEN:if(w){
      if(w->xsurface)wlr_xwayland_surface_set_fullscreen(w->xsurface,c->a!=0);
      else if(w->toplevel&&w->toplevel->base->initialized)wlr_xdg_toplevel_set_fullscreen(w->toplevel,c->a!=0);
    }break;
    case CMD_CLOSE:if(w&&w->toplevel){clear_window_pointer(w);wlr_xdg_toplevel_send_close(w->toplevel);}else if(w&&w->xsurface){clear_window_pointer(w);close_xwindow(w);}break;
    case CMD_STOP:atomic_store(&server.stopping,true);break;
    }free(c);
  }
}
static void handle_add_xwayland(int session,uid_t uid) {
  pthread_mutex_lock(&server.mutex);
  server.xw_ok=false;server.xw_display[0]=0;
  if(atomic_load(&server.started)&&!atomic_load(&server.stopping)&&server.xwayland_path[0]){
    struct XwaylandSession* xw=calloc(1,sizeof(*xw));
    if(xw){
      xw->session=session;xw->uid=uid;
      xw->process_fd=-1;
      /* wlr_xwayland_create also creates the xwayland_shell_v1 global that
       * xwm_create needs; rootless Xwayland emulates RandR modes by itself. */
      xw->xwayland=wlr_xwayland_create(server.display,server.compositor,true);
      if(xw->xwayland){
        wlr_xwayland_set_seat(xw->xwayland,server.seat);
        xw->ready.notify=on_xwayland_ready;wl_signal_add(&xw->xwayland->events.ready,&xw->ready);
        xw->new_surface.notify=on_xwayland_new_surface;wl_signal_add(&xw->xwayland->events.new_surface,&xw->new_surface);
        xw->destroy.notify=on_xwayland_destroy;wl_signal_add(&xw->xwayland->events.destroy,&xw->destroy);
        xw->next=server.xwayland_sessions;server.xwayland_sessions=xw;
        xwayland_apply_socket_mode(xw);
        snprintf(server.xw_display,sizeof(server.xw_display),"X%s",
            xw->xwayland->display_name+1);
        server.xw_ok=true;
      }else{free(xw);}
    }
  }
  if(!server.xw_ok)
    __android_log_print(ANDROID_LOG_ERROR,"MatonCompositor","Session %d: Xwayland creation failed",session);
  server.xw_completed++;
  pthread_cond_broadcast(&server.xw_cond);
  pthread_mutex_unlock(&server.mutex);
}
static int on_event_fd(int fd,uint32_t mask,void* data){(void)fd;(void)mask;(void)data;process_commands();return 0;}
/* GPU clients hand over dma-bufs that SurfaceFlinger shows as they are
 * (presenter.c, dmabuf_import.c). Only what gralloc can fully describe is offered:
 * single-plane RGB formats, linear or the driver's implicit layout, on the
 * render node SurfaceFlinger's GPU uses. */
/* Wayland caches SO_PEERCRED when constructing the client. Check every
 * connection, including session sockets and Xwayland's socketpair. */
static void on_new_client(struct wl_listener* listener,void* data) {
  (void)listener;struct wl_client* client=data;uid_t uid;
  wl_client_get_credentials(client,NULL,&uid,NULL);
  if(uid!=getuid())wl_client_destroy(client);
}
/* Every session has its own Xwayland and so its own xwayland_shell_v1 global,
 * which admits only that Xwayland. Advertising all of them lets one Xwayland
 * bind another's and fail ("Permission denied to bind to xwayland_shell_v1"),
 * so each is shown to its own Xwayland only (as wlroots' xwayland.h advises). */
static bool global_filter(const struct wl_client* client,const struct wl_global* global,void* data) {
  (void)data;
  if(strcmp(wl_global_get_interface(global)->name,"xwayland_shell_v1"))return true;
  for(struct XwaylandSession* xw=server.xwayland_sessions;xw;xw=xw->next){
    struct wlr_xwayland* x=xw->xwayland;
    if(!x||!x->shell_v1||x->shell_v1->global!=global)continue;
    return x->server&&x->server->client==client;
  }
  /* wlr_xwayland_create() advertises a new session's shell global before
   * the session is listed; other Xwaylands that bound it then were refused
   * at bind time and died with a protocol error. Unowned means hidden. */
  return false;
}
static bool render_node(dev_t* out) {
  DIR* dir=opendir("/dev/dri");if(!dir)return false;
  struct dirent* e;bool found=false;
  while(!found&&(e=readdir(dir))){
    if(strncmp(e->d_name,"renderD",7))continue;
    char path[64];snprintf(path,sizeof(path),"/dev/dri/%s",e->d_name);
    struct stat st;if(!stat(path,&st)&&S_ISCHR(st.st_mode)){*out=st.st_rdev;found=true;}
  }
  closedir(dir);return found;
}
static bool check_dmabuf(struct wlr_dmabuf_attributes* attrs,void* data) {
  (void)data;
  /* Replaces wlroots' own check: require the runtime-probed format and
   * complete plane metadata for this actual import, not just import success. */
  return attrs->n_planes==1&&maton_dmabuf_format_supported(attrs->format)&&maton_dmabuf_importable(attrs);
}
static bool create_linux_dmabuf(void) {
  static const uint32_t formats[]={DRM_FORMAT_ARGB8888,DRM_FORMAT_XRGB8888,DRM_FORMAT_ABGR8888,DRM_FORMAT_XBGR8888,DRM_FORMAT_RGB565};
  for(size_t i=0;i<sizeof(formats)/sizeof(formats[0]);++i)
    if(maton_dmabuf_format_supported(formats[i])&&
        (!wlr_drm_format_set_add(&server.dmabuf_formats,formats[i],DRM_FORMAT_MOD_LINEAR)||
        !wlr_drm_format_set_add(&server.dmabuf_formats,formats[i],DRM_FORMAT_MOD_INVALID)))return false;
  if(server.dmabuf_formats.len==0){
    __android_log_print(ANDROID_LOG_WARN,"MatonCompositor","No mapper-safe formats; linux-dmabuf disabled");
    return true;
  }
  dev_t device;
  if(!render_node(&device)){
    /* No GPU: clients stay on shared memory. */
    __android_log_print(ANDROID_LOG_WARN,"MatonCompositor","No DRM render node; linux-dmabuf disabled");
    return true;
  }
  struct wlr_linux_dmabuf_feedback_v1 feedback={.main_device=device};
  wl_array_init(&feedback.tranches);
  struct wlr_linux_dmabuf_feedback_v1_tranche* tranche=wlr_linux_dmabuf_feedback_add_tranche(&feedback);
  struct wlr_drm_format_set none={0};
  bool ok=tranche&&wlr_drm_format_set_union(&tranche->formats,&none,&server.dmabuf_formats);
  if(ok)tranche->target_device=device;
  struct wlr_linux_dmabuf_v1* dmabuf=ok?wlr_linux_dmabuf_v1_create(server.display,4,&feedback):NULL;
  wlr_linux_dmabuf_feedback_v1_finish(&feedback);
  if(!dmabuf)return false;
  wlr_linux_dmabuf_v1_set_check_dmabuf_callback(dmabuf,check_dmabuf,NULL);
  return true;
}
static void* server_main(void* unused) {
  (void)unused;bool initialized=false;const char* stage="display";
  wlr_log_init(WLR_ERROR,maton_wlr_log);
  server.display=wl_display_create();
  if(!server.display)goto done;server.loop=wl_display_get_event_loop(server.display);
  wl_list_init(&server.sync_surfaces);
  server.sync_file_global=wl_global_create(server.display,&maton_sync_file_interface,1,NULL,bind_sync_file);
  if(!server.sync_file_global)__android_log_print(ANDROID_LOG_WARN,"MatonCompositor","Vulkan sync_file protocol unavailable");
  if(!maton_ahb_buffer_global_create(server.display))
    __android_log_print(ANDROID_LOG_WARN,"MatonCompositor","AHardwareBuffer transport protocol unavailable");
  server.wsi_trace_count=0;
  wl_display_add_protocol_logger(server.display,trace_wsi,&server.wsi_trace_count);
  server.new_client.notify=on_new_client;wl_display_add_client_created_listener(server.display,&server.new_client);
  maton_surface_output_set_release_dispatch(release_buffer);
  stage="event loop";server.event_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
  if(server.event_fd<0||!wl_event_loop_add_fd(server.loop,server.event_fd,WL_EVENT_READABLE,on_event_fd,NULL))goto done;
  stage="backend and renderer";server.backend=wlr_headless_backend_create(server.loop);server.renderer=maton_renderer_create(wlr_pixman_renderer_create(),&server.dmabuf_formats);
  if(!server.backend||!server.renderer||!maton_ahb_allocator_init(&server.ahb_allocator))goto done;
  stage="renderer globals";if(!wlr_renderer_init_wl_display(server.renderer,server.display))goto done;
  stage="linux-dmabuf";if(!create_linux_dmabuf())goto done;
  wl_display_set_global_filter(server.display,global_filter,NULL);
  server.allocator=maton_ahb_allocator_base(&server.ahb_allocator);
  if(!maton_egl_uploader_init(&server.uploader))__android_log_print(ANDROID_LOG_WARN,"MatonCompositor","EGL upload fallback unavailable");
  wlr_subcompositor_create(server.display);wlr_viewporter_create(server.display);
  if(!wlr_data_device_manager_create(server.display))goto done;
  server.xdg_shell=wlr_xdg_shell_create(server.display,6);server.compositor=wlr_compositor_create(server.display,6,server.renderer);server.seat=wlr_seat_create(server.display,"seat0");
  stage="shell and seat";if(!server.xdg_shell||!server.compositor||!server.seat)goto done;
  /* Default to server decoration only when the client requests no mode. */
  stage="pointer constraints";
  server.constraints=wlr_pointer_constraints_v1_create(server.display);
  server.relative=wlr_relative_pointer_manager_v1_create(server.display);
  if(!server.constraints||!server.relative)goto done;
  server.new_constraint.notify=on_new_constraint;wl_signal_add(&server.constraints->events.new_constraint,&server.new_constraint);
  server.pointer_focus_change.notify=on_pointer_focus_change;wl_signal_add(&server.seat->pointer_state.events.focus_change,&server.pointer_focus_change);
  stage="decorations";
  struct wlr_server_decoration_manager* legacy_decoration=wlr_server_decoration_manager_create(server.display);
  struct wlr_xdg_decoration_manager_v1* decorations=wlr_xdg_decoration_manager_v1_create(server.display);
  if(!legacy_decoration||!decorations)goto done;
  wlr_server_decoration_manager_set_default_mode(legacy_decoration,WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
  server.new_decoration.notify=on_new_decoration;wl_signal_add(&decorations->events.new_toplevel_decoration,&server.new_decoration);
  wlr_keyboard_init(&server.keyboard,NULL,"maton-keyboard");server.keyboard_initialized=1;
  /* The keymap is self-contained. Android has no host XKB include tree,
   * so default include lookup would make context creation fail. */
  stage="keyboard keymap";
  struct xkb_context* xc=xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);struct xkb_keymap* km=xc?xkb_keymap_new_from_string(xc,kMatonUsKeymap,XKB_KEYMAP_FORMAT_TEXT_V1,XKB_KEYMAP_COMPILE_NO_FLAGS):NULL;
  bool keymap_ready=km&&wlr_keyboard_set_keymap(&server.keyboard,km);
  if(km)xkb_keymap_unref(km);if(xc)xkb_context_unref(xc);
  if(!keymap_ready)goto done;
  wlr_seat_set_keyboard(server.seat,&server.keyboard);wlr_seat_set_capabilities(server.seat,WL_SEAT_CAPABILITY_KEYBOARD|WL_SEAT_CAPABILITY_POINTER);
  server.new_toplevel.notify=on_new_toplevel;wl_signal_add(&server.xdg_shell->events.new_toplevel,&server.new_toplevel);
  stage="virtual monitor";struct wlr_output* monitor=wlr_headless_add_output(server.backend,1280,720);
  /* This output advertises geometry only; Android window outputs render.
   * Leaving its renderer unset avoids allocating an unused scanout buffer. */
  if(!monitor)goto done;
  wlr_output_set_name(monitor,"MatonOS");
  struct wlr_output_state monitor_state;wlr_output_state_init(&monitor_state);
  wlr_output_state_set_enabled(&monitor_state,true);wlr_output_state_set_custom_mode(&monitor_state,1280,720,60000);
  bool monitor_ok=wlr_output_commit_state(monitor,&monitor_state);wlr_output_state_finish(&monitor_state);
  if(!monitor_ok)goto done;
  server.monitor=monitor;
  /* SDL's Wayland backend needs logical monitor geometry for scaling. The
   * layout owns the wl_output global and is destroyed with the display. */
  stage="logical monitor";struct wlr_output_layout* layout=wlr_output_layout_create(server.display);
  if(!layout||!wlr_output_layout_add(layout,monitor,0,0)||
      !wlr_xdg_output_manager_v1_create(server.display,layout))goto done;
  server.new_popup.notify=on_new_popup;wl_signal_add(&server.xdg_shell->events.new_popup,&server.new_popup);
  stage="Wayland socket";if(wl_display_add_socket(server.display,server.socket_name)<0)goto done;
  stage="backend start";if(!wlr_backend_start(server.backend))goto done;
  char socket_path[640];snprintf(socket_path,sizeof(socket_path),"%s/%s",server.runtime_dir,server.socket_name);
  stage="socket permissions";if(chmod(socket_path,0600))goto done;
  initialized=true;
done:
  if(!initialized)__android_log_print(ANDROID_LOG_ERROR,"MatonCompositor","Startup failed at %s (errno=%d)",stage,errno);
  pthread_mutex_lock(&server.mutex);server.ok=initialized;server.ready=1;pthread_cond_broadcast(&server.ready_cond);pthread_mutex_unlock(&server.mutex);
  if(server.ok){while(!atomic_load(&server.stopping)){wl_display_flush_clients(server.display);wl_event_loop_dispatch(server.loop,-1);}wl_display_destroy_clients(server.display);}
  while(server.xwayland_sessions){struct XwaylandSession* xw=server.xwayland_sessions;server.xwayland_sessions=xw->next;xw->next=NULL;if(xw->xwayland){/* The destroy listener unlinks and frees the session. */wlr_xwayland_destroy(xw->xwayland);}else{free(xw);}}
  while(server.windows)destroy_window(server.windows);
  if(server.new_popup.link.prev){wl_list_remove(&server.new_popup.link);wl_list_init(&server.new_popup.link);}
  if(server.new_decoration.link.prev){wl_list_remove(&server.new_decoration.link);wl_list_init(&server.new_decoration.link);}
  if(server.new_toplevel.link.prev){wl_list_remove(&server.new_toplevel.link);wl_list_init(&server.new_toplevel.link);}
  if(server.new_constraint.link.prev){wl_list_remove(&server.new_constraint.link);wl_list_init(&server.new_constraint.link);}
  if(server.pointer_focus_change.link.prev){wl_list_remove(&server.pointer_focus_change.link);wl_list_init(&server.pointer_focus_change.link);}
  server.constraint=NULL;server.constraints=NULL;server.relative=NULL;
  if(server.keyboard_initialized)wlr_keyboard_finish(&server.keyboard);server.keyboard_initialized=0;
  if(server.seat)wlr_seat_destroy(server.seat);server.seat=NULL;server.pointer_buttons=0;server.pointer_focus=NULL;server.pointer_window=0;
  if(server.allocator)wlr_allocator_destroy(server.allocator);server.allocator=NULL;
  if(server.renderer)wlr_renderer_destroy(server.renderer);server.renderer=NULL;
  /* The renderer reports these as its dma-buf formats: free it after. */
  wlr_drm_format_set_finish(&server.dmabuf_formats);
  maton_egl_uploader_finish(&server.uploader);if(server.backend)wlr_backend_destroy(server.backend);server.backend=NULL;
  if(server.display)wl_display_destroy(server.display);server.display=NULL;server.loop=NULL;
  while(server.sessions){struct SocketSession* s=server.sessions;server.sessions=s->next;unlink(s->path);if(s->directory_fd>=0)close(s->directory_fd);free(s);}
  if(server.event_fd>=0)close(server.event_fd);server.event_fd=-1;
  if(server.runtime_dir_fd>=0)close(server.runtime_dir_fd);server.runtime_dir_fd=-1;
  if(server.xwayland_dir_fd>=0)close(server.xwayland_dir_fd);server.xwayland_dir_fd=-1;
  return NULL;
}
bool maton_core_start(const char* socket_name,const char* runtime_dir){
  bool expected=false;if(!atomic_compare_exchange_strong(&server.started,&expected,true)){pthread_mutex_lock(&server.mutex);bool ok=server.ok;pthread_mutex_unlock(&server.mutex);return ok;}
  if(!socket_name||!runtime_dir||strlen(socket_name)>=sizeof(server.socket_name)||strlen(runtime_dir)>=sizeof(server.runtime_dir))goto fail;
  strcpy(server.socket_name,socket_name);
  if(mkdir(runtime_dir,0700)&&errno!=EEXIST)goto fail;
  if(chmod(runtime_dir,0700))goto fail;
  server.runtime_dir_fd=maton_socket_dir_open_inherited(runtime_dir);
  if(server.runtime_dir_fd<0)goto fail;
  if(maton_socket_dir_path(server.runtime_dir,sizeof(server.runtime_dir),server.runtime_dir_fd)<0||setenv("XDG_RUNTIME_DIR",server.runtime_dir,1))goto fail;
  char path[640];snprintf(path,sizeof(path),"%s/%s",runtime_dir,socket_name);unlink(path);
  pthread_mutex_lock(&server.mutex);server.ready=server.ok=0;pthread_mutex_unlock(&server.mutex);
  atomic_store(&server.stopping,false);
  if(pthread_create(&server.thread,NULL,server_main,NULL))goto fail;
  pthread_mutex_lock(&server.mutex);struct timespec limit;clock_gettime(CLOCK_REALTIME,&limit);limit.tv_sec+=2;while(!server.ready&&pthread_cond_timedwait(&server.ready_cond,&server.mutex,&limit)==0){}int ok=server.ready&&server.ok;if(!ok){
    /* A late successful startup must skip its loop, or wake if it already
     * entered dispatch. ready publishes event_fd while holding this mutex. */
    atomic_store(&server.stopping,true);
    if(server.ready&&server.ok&&server.event_fd>=0){uint64_t one=1;(void)write(server.event_fd,&one,sizeof(one));}
  }
  pthread_mutex_unlock(&server.mutex);if(!ok){pthread_join(server.thread,NULL);pthread_mutex_lock(&server.mutex);server.started=false;server.ready=server.ok=0;pthread_mutex_unlock(&server.mutex);}return ok;
fail:
  if(server.runtime_dir_fd>=0){close(server.runtime_dir_fd);server.runtime_dir_fd=-1;}
  atomic_store(&server.started,false);return false;
}
void maton_core_stop(void){if(!atomic_load(&server.started))return;atomic_store(&server.stopping,true);if(server.event_fd>=0){uint64_t one=1;(void)write(server.event_fd,&one,sizeof(one));}pthread_join(server.thread,NULL);pthread_mutex_lock(&server.mutex);server.ready=server.ok=0;atomic_store(&server.started,false);pthread_mutex_unlock(&server.mutex);atomic_store(&server.stopping,false);atomic_store(&server.demo_started,false);}
void maton_core_launch_demo(void){bool expected=false;if(atomic_compare_exchange_strong(&server.demo_started,&expected,true)){pthread_t t;if(!pthread_create(&t,NULL,maton_wayland_test_client,NULL))pthread_detach(t);}}
void maton_core_attach(int id,struct MatonSurfaceOutput* output,int width,int height){command(CMD_ATTACH,id,width,height,0,0,0,0,0,0,output,NULL);}
void maton_core_detach(int id){command(CMD_DETACH,id,0,0,0,0,0,0,0,0,NULL,NULL);}
void maton_core_resize(int id,int width,int height){command(CMD_RESIZE,id,width,height,0,0,0,0,0,0,NULL,NULL);}
void maton_core_set_fullscreen(int id,int on){command(CMD_FULLSCREEN,id,on,0,0,0,0,0,0,0,NULL,NULL);}
void maton_core_key(int id,int keycode,int scan,int action,int meta,int64_t time){(void)meta;command(CMD_KEY,id,keycode,scan,action,time,0,0,0,0,NULL,NULL);}
void maton_core_motion(int id,float x,float y,float vs,float hs,int action,int buttons,int64_t time){command(CMD_MOTION,id,action,buttons,0,time,x,y,vs,hs,NULL,NULL);}

void maton_core_stopped(int id,bool stopped){command(CMD_STOPPED,id,stopped,0,0,0,0,0,0,0,NULL,NULL);}
void maton_core_close(int id){command(CMD_CLOSE,id,0,0,0,0,0,0,0,0,NULL,NULL);}

bool maton_core_xwayland_init(const char* socket_dir,const char* xwayland_path) {
  if(!socket_dir||!xwayland_path||strlen(socket_dir)>=sizeof(server.xwayland_dir)||
      strlen(xwayland_path)>=sizeof(server.xwayland_path))return false;
  strcpy(server.xwayland_dir,socket_dir);
  server.xwayland_dir_fd=open(socket_dir,O_PATH|O_DIRECTORY);
  if(server.xwayland_dir_fd<0)return false;
  if(maton_socket_dir_path(server.xwayland_dir,sizeof(server.xwayland_dir),server.xwayland_dir_fd)<0)return false;
  strcpy(server.xwayland_path,xwayland_path);
  /* Only consulted from the Wayland thread; Xwayland sessions are created
   * after the server starts, so the environment is in place in time. */
  if(prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0)<0)return false;
  if(setenv("WLR_XWAYLAND_SOCKET_DIR",socket_dir,1))return false;
  /* An app process's stderr goes nowhere; Xwayland reports fatal startup
   * errors only there. Keep them in the app's runtime dir (<dir>/../xwayland.log). */
  {
    char log_path[PATH_MAX];
    if(snprintf(log_path,sizeof(log_path),"%s/../xwayland.log",socket_dir)<(int)sizeof(log_path)){
      int log_fd=open(log_path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);
      if(log_fd>=0){dup2(log_fd,STDERR_FILENO);close(log_fd);}
    }
  }
  /* wlroots bakes the build-host Xwayland path from xwayland.pc into its
   * binary check and exec; WLR_XWAYLAND overrides both at runtime. */
  char binary[512];
  int written=snprintf(binary,sizeof(binary),"%s/libmatonos-Xwayland.so",xwayland_path);
  if(written<0||written>=(int)sizeof(binary))return false;
  /* Per-app clients share Xwayland's UID and use a mode-0600 socket. */
  /* Private symlinks resolve APK library names, including the GL forwarders. */
  const char* libraries=getenv("MATON_XWAYLAND_LIBDIR");
  if(!libraries||setenv("LD_LIBRARY_PATH",libraries,1))return false;
  /* Mesa's Android build has no desktop GL: glamor's default GL-first probe
   * makes libepoxy dlopen libGLESv1_CM and abort, so ask for GLES directly. */
  return setenv("WLR_XWAYLAND",binary,1)==0&&
      setenv("WLR_XWAYLAND_GLAMOR","es",1)==0&&
      setenv("WLR_XWAYLAND_NO_ABSTRACT","1",1)==0&&
      setenv("WLR_XWAYLAND_NO_ACCESS_CONTROL","1",1)==0;
}
bool maton_core_add_xwayland(int session,int uid,char* display,size_t display_size) {
  if(session<=0||!display||display_size==0)return false;
  pthread_mutex_lock(&server.mutex);
  if(!atomic_load(&server.started)||atomic_load(&server.stopping)||!server.xwayland_path[0]){
    pthread_mutex_unlock(&server.mutex);return false;
  }
  unsigned sent=++server.xw_sent;
  pthread_mutex_unlock(&server.mutex);
  struct Command* c=calloc(1,sizeof(*c));
  if(!c)return false;
  c->kind=CMD_ADD_XWAYLAND;c->a=session;c->b=uid;enqueue(c);
  pthread_mutex_lock(&server.mutex);
  struct timespec limit;clock_gettime(CLOCK_REALTIME,&limit);limit.tv_sec+=5;
  while(server.xw_completed<sent){
    if(pthread_cond_timedwait(&server.xw_cond,&server.mutex,&limit))break;
  }
  bool ok=server.xw_completed>=sent&&server.xw_ok;
  if(ok)snprintf(display,display_size,"%s",server.xw_display);
  pthread_mutex_unlock(&server.mutex);
  return ok;
}
bool maton_core_add_session(int id,const char* path) {
  if(id<=0||!path||strlen(path)>=sizeof(((struct SocketSession*)0)->path)||
      !atomic_load(&server.started)||atomic_load(&server.stopping))return false;
  struct SocketSession* s=calloc(1,sizeof(*s));struct Command* c=calloc(1,sizeof(*c));
  if(!s||!c){free(s);free(c);return false;}
  s->id=id;strcpy(s->path,path);s->directory_fd=-1;
  s->fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
  char directory[PATH_MAX];snprintf(directory,sizeof(directory),"%s",path);char *slash=strrchr(directory,'/');
  if(!slash||!slash[1]){free(s);free(c);return false;}const char *name=slash+1;*slash=0;
  s->directory_fd=maton_socket_dir_open(directory);
  struct sockaddr_un address={.sun_family=AF_UNIX};
  if(s->directory_fd<0||maton_socket_path(address.sun_path,sizeof(address.sun_path),s->directory_fd,name)<0){if(s->directory_fd>=0)close(s->directory_fd);free(s);free(c);return false;}
  unlink(path);
  if(s->fd<0||bind(s->fd,(struct sockaddr*)&address,sizeof(address))||chmod(path,0600)||listen(s->fd,128)){
    if(s->fd>=0)close(s->fd);close(s->directory_fd);unlink(path);free(s);free(c);return false;
  }
  /* Accept registration runs on the Wayland thread. Connections arriving
   * before it runs wait in this listening socket's backlog. */
  c->kind=CMD_ADD_SESSION;c->session=s;enqueue(c);return true;
}

void maton_core_capture_lost(int id){struct Command* c=calloc(1,sizeof(*c));if(c){c->kind=CMD_CAPTURE_LOST;c->id=id;enqueue(c);}}
void maton_core_relative(int id,float x,float y,float vs,float hs,int buttons,int64_t time){struct Command* c=calloc(1,sizeof(*c));if(c){c->kind=CMD_RELATIVE;c->id=id;c->x=x;c->y=y;c->vs=vs;c->hs=hs;c->b=buttons;c->time=time;enqueue(c);}}
