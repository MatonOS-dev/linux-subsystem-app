/* Native transport/lifetime test. No SurfaceControl, no presentation claim. */
#define _GNU_SOURCE
#ifndef MATON_AHB_LINUX_CLIENT
#include "ahb_protocol.h"
#include "maton_renderer.h"
#endif
#include "maton-ahb-v1-client-protocol.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#ifndef MATON_AHB_LINUX_CLIENT
#include <wayland-server-core.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#else
#include "runtime.h"
#include "backend.h"
#include "bridge.h"
#include "wsi.h"
/* Stable public NDK description ABI; this client links Linux libc only. */
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct {
  uint32_t width,height,layers,format;
  uint64_t usage;
  uint32_t stride,rfu0;
  uint64_t rfu1;
} AHardwareBuffer_Desc;
static struct adopt_api* adoption;
static int fast;
static struct mv_wsi_driver driver;
static PFN_vkDestroyDevice destroy_device;
static VkInstance vk_instance;
static PFN_vkDestroyInstance destroy_instance;
static int (*receive_handle)(int,AHardwareBuffer**);
static void (*describe_handle)(const AHardwareBuffer*,AHardwareBuffer_Desc*);
static void (*release_handle)(AHardwareBuffer*);
static void resident_init(void) {
  struct gpu_bridge* bridge=(void*)(uintptr_t)strtoull(getenv("MATON_GPU_BRIDGE"),NULL,16);
  adoption=(void*)(uintptr_t)strtoull(getenv("MATON_THREAD_ADOPT"),NULL,16);
  assert(bridge && bridge->version==1 && adoption);
  fast=mv_fast_tls();struct mv_scope scope;assert(switch_to_android(adoption,fast,&scope));
  void* lib=bridge->open("libnativewindow.so",2);
  if(lib){
    receive_handle=(void*)bridge->symbol(lib,"AHardwareBuffer_recvHandleFromUnixSocket");
    describe_handle=(void*)bridge->symbol(lib,"AHardwareBuffer_describe");
    release_handle=(void*)bridge->symbol(lib,"AHardwareBuffer_release");
  }
  void* vulkan=bridge->open("/system/lib64/libvulkan.so",2);
  PFN_vkGetInstanceProcAddr gipa=vulkan?(void*)bridge->symbol(vulkan,"vkGetInstanceProcAddr"):NULL;
  VkResult result=VK_ERROR_INITIALIZATION_FAILED;
  if(gipa){
    PFN_vkCreateInstance create_instance=(void*)gipa(NULL,"vkCreateInstance");
    VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
    result=create_instance(&ci,NULL,&vk_instance);
    if(!result){
      destroy_instance=(void*)gipa(vk_instance,"vkDestroyInstance");
      PFN_vkEnumeratePhysicalDevices enumerate=(void*)gipa(vk_instance,"vkEnumeratePhysicalDevices");
      VkPhysicalDevice physical=NULL;uint32_t count=1;result=enumerate(vk_instance,&count,&physical);
      if(result==VK_INCOMPLETE)result=VK_SUCCESS;
      if(!result && !count)result=VK_ERROR_INITIALIZATION_FAILED;
      if(!result){
        PFN_vkGetPhysicalDeviceImageFormatProperties2 query=(void*)gipa(vk_instance,"vkGetPhysicalDeviceImageFormatProperties2");
        VkPhysicalDeviceExternalImageFormatInfo ex={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
        VkPhysicalDeviceImageFormatInfo2 info={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,.pNext=&ex,.format=VK_FORMAT_R8G8B8A8_UNORM,.type=VK_IMAGE_TYPE_2D,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        VkExternalImageFormatProperties ep={.sType=VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 props={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,.pNext=&ep};
        result=query(physical,&info,&props);
        if(!result && !(ep.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))result=VK_ERROR_FORMAT_NOT_SUPPORTED;
      }
      if(!result){
        float priority=1;
        VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=0,.queueCount=1,.pQueuePriorities=&priority};
        const char* extension="VK_ANDROID_external_memory_android_hardware_buffer";
        VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.enabledExtensionCount=1,.ppEnabledExtensionNames=&extension};
        PFN_vkCreateDevice create_device=(void*)gipa(vk_instance,"vkCreateDevice");
        result=create_device(physical,&di,NULL,&driver.device);
        if(!result){
          PFN_vkGetDeviceProcAddr gdpa=(void*)gipa(vk_instance,"vkGetDeviceProcAddr");
          driver.properties=(void*)gdpa(driver.device,"vkGetAndroidHardwareBufferPropertiesANDROID");
          driver.create=(void*)gdpa(driver.device,"vkCreateImage");
          driver.allocate=(void*)gdpa(driver.device,"vkAllocateMemory");
          driver.bind=(void*)gdpa(driver.device,"vkBindImageMemory");
          driver.destroy=(void*)gdpa(driver.device,"vkDestroyImage");
          driver.free=(void*)gdpa(driver.device,"vkFreeMemory");
          destroy_device=(void*)gdpa(driver.device,"vkDestroyDevice");
        }
      }
    }
  }
  switch_back(&scope);assert(receive_handle && describe_handle && release_handle && result==VK_SUCCESS);
}
static int AHardwareBuffer_recvHandleFromUnixSocket(int fd,AHardwareBuffer** out) {
  struct mv_scope scope;assert(switch_to_android(adoption,fast,&scope));
  int result=receive_handle(fd,out);switch_back(&scope);return result;
}
static void AHardwareBuffer_describe(AHardwareBuffer* buffer,AHardwareBuffer_Desc* desc) {
  struct mv_scope scope;assert(switch_to_android(adoption,fast,&scope));
  describe_handle(buffer,desc);switch_back(&scope);
}
static void AHardwareBuffer_release(AHardwareBuffer* buffer) {
  struct mv_scope scope;assert(switch_to_android(adoption,fast,&scope));
  release_handle(buffer);switch_back(&scope);
}
#endif

static struct wl_compositor* compositor;
static struct maton_ahb_manager_v1* manager;
static struct wl_buffer* buffers[8];
static unsigned allocated, failed, releases, last_reason;
static int transport=-1;
static void registry_global(void* data,struct wl_registry* registry,uint32_t name,const char* interface,uint32_t version) {
  (void)data;(void)version;
  if(!strcmp(interface,"wl_compositor"))compositor=wl_registry_bind(registry,name,&wl_compositor_interface,4);
  if(!strcmp(interface,"maton_ahb_manager_v1"))manager=wl_registry_bind(registry,name,&maton_ahb_manager_v1_interface,1);
}
static void registry_remove(void* data,struct wl_registry* registry,uint32_t name) {(void)data;(void)registry;(void)name;}
static const struct wl_registry_listener registry_listener={registry_global,registry_remove};
static void on_transport(void* data,struct maton_ahb_session_v1* session,int32_t fd,uint32_t dimension,uint32_t count,uint32_t bytes) {
  (void)data;(void)session;assert(dimension==4096 && count==8 && bytes==64*1024*1024);transport=fd;
}
static unsigned formats;
static void on_format(void* data,struct maton_ahb_session_v1* session,uint32_t format) {(void)data;(void)session;formats|=1u<<format;}
static void on_ready(void* data,struct maton_ahb_session_v1* session) {(void)data;(void)session;}
static void on_release(void* data,struct wl_buffer* buffer) {(void)data;(void)buffer;releases++;}
static const struct wl_buffer_listener buffer_listener={on_release};
static void on_allocated(void* data,struct maton_ahb_session_v1* session,uint32_t request,struct wl_buffer* buffer,uint32_t usage_lo,uint32_t usage_hi) {
  (void)data;(void)session;(void)request;(void)usage_hi;
  assert(allocated<8 && transport>=0);
  struct pollfd p={.fd=transport,.events=POLLIN};assert(poll(&p,1,1000)==1);
  AHardwareBuffer* ahb=NULL;
  assert(AHardwareBuffer_recvHandleFromUnixSocket(transport,&ahb)==0 && ahb);
  AHardwareBuffer_Desc desc;AHardwareBuffer_describe(ahb,&desc);
  assert(desc.width==64 && desc.height==64 && desc.layers==1 && desc.format==1);
  assert((desc.usage&usage_lo)==usage_lo);
#ifdef MATON_AHB_LINUX_CLIENT
  struct mv_scope scope;assert(switch_to_android(adoption,fast,&scope));
  VkImage image=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
  VkResult imported=mv_wsi_import_ahb(&driver,ahb,(VkExtent2D){64,64},VK_FORMAT_R8G8B8A8_UNORM,&image,&memory);
  if(image)driver.destroy(driver.device,image,NULL);
  if(memory)driver.free(driver.device,memory,NULL);
  switch_back(&scope);assert(imported==VK_SUCCESS);
#endif
  AHardwareBuffer_release(ahb);
  buffers[allocated++]=buffer;assert(wl_buffer_add_listener(buffer,&buffer_listener,NULL)==0);
}
static void on_failed(void* data,struct maton_ahb_session_v1* session,uint32_t request,uint32_t reason) {
  (void)data;(void)session;(void)request;failed++;last_reason=reason;
}
static const struct maton_ahb_session_v1_listener session_listener={on_transport,on_format,on_ready,on_allocated,on_failed};

#ifndef MATON_AHB_LINUX_CLIENT
static struct wl_event_loop* server_loop;
static struct wlr_buffer* held;
static struct wl_listener new_surface,commit,surface_gone;
static struct wl_event_source* timer;
static int release_held(void* data) {
  (void)data;assert(held);wlr_buffer_unlock(held);held=NULL;return 0;
}
static void on_commit(struct wl_listener* listener,void* data) {
  (void)listener;
  struct wlr_surface* surface=data;
  if(surface->buffer){
    assert(!held);
    assert(maton_ahb_protocol_buffer(&surface->buffer->base));
    held=wlr_buffer_lock(&surface->buffer->base);
    assert(!wl_event_source_timer_update(timer,250));
  }
}
static void on_surface_gone(struct wl_listener* listener,void* data) {
  (void)listener;(void)data;wl_list_remove(&commit.link);wl_list_remove(&surface_gone.link);
}
static void on_new_surface(struct wl_listener* listener,void* data) {
  (void)listener;struct wlr_surface* surface=data;
  surface_gone.notify=on_surface_gone;wl_signal_add(&surface->events.destroy,&surface_gone);
  commit.notify=on_commit;wl_signal_add(&surface->events.commit,&commit);
}
static void server(int fd) {
  struct wl_display* display=wl_display_create();assert(display);
  server_loop=wl_display_get_event_loop(display);
  struct wlr_renderer* renderer=maton_renderer_create(wlr_pixman_renderer_create(),NULL);assert(renderer);
  assert(wlr_renderer_init_wl_display(renderer,display));
  struct wlr_compositor* c=wlr_compositor_create(display,4,renderer);assert(c);
  new_surface.notify=on_new_surface;wl_signal_add(&c->events.new_surface,&new_surface);
  timer=wl_event_loop_add_timer(server_loop,release_held,NULL);assert(timer);
  assert(maton_ahb_protocol_create(display));assert(wl_client_create(display,fd));
  for(;;){wl_display_flush_clients(display);assert(!wl_event_loop_dispatch(server_loop,-1));}
}
#endif
int main(int argc,char** argv) {
#ifndef MATON_AHB_LINUX_CLIENT
  int fds[2];assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,fds));
  pid_t pid=fork();assert(pid>=0);
  if(!pid){
    pid_t parent=getppid();assert(!prctl(PR_SET_PDEATHSIG,SIGTERM));
    if(getppid()!=parent)_exit(1);
    close(fds[0]);server(fds[1]);return 1;
  }
  close(fds[1]);
  if(argc>1){
    assert(fcntl(fds[0],F_SETFD,0)==0);
    char value[32];snprintf(value,sizeof(value),"%d",fds[0]);assert(!setenv("WAYLAND_SOCKET",value,1));
    snprintf(value,sizeof(value),"%d",pid);assert(!setenv("MATON_TEST_SERVER_PID",value,1));
    execv(argv[1],argv+1);perror("exec launcher");kill(pid,SIGTERM);return 1;
  }
  int wayland_fd=fds[0];
#else
  (void)argc;(void)argv;resident_init();
  int wayland_fd=atoi(getenv("WAYLAND_SOCKET"));
  pid_t pid=(pid_t)atoi(getenv("MATON_TEST_SERVER_PID"));assert(pid>0);
#endif
  struct wl_display* display=wl_display_connect_to_fd(wayland_fd);assert(display);
  struct wl_registry* registry=wl_display_get_registry(display);wl_registry_add_listener(registry,&registry_listener,NULL);
  assert(wl_display_roundtrip(display)>=0 && compositor && manager);
  struct wl_surface* surface=wl_compositor_create_surface(compositor);
  struct maton_ahb_session_v1* session=maton_ahb_manager_v1_get_session(manager,surface);
  maton_ahb_session_v1_add_listener(session,&session_listener,NULL);
  assert(wl_display_roundtrip(display)>=0 && transport>=0 && (formats&2));
  maton_ahb_session_v1_create_buffer(session,1,0,64,1);
  assert(wl_display_roundtrip(display)>=0 && failed==1 && last_reason==MATON_AHB_SESSION_V1_FAILURE_INVALID);
  for(unsigned i=0;i<8;i++)maton_ahb_session_v1_create_buffer(session,i+2,64,64,1);
  assert(wl_display_roundtrip(display)>=0 && allocated==8);
  maton_ahb_session_v1_create_buffer(session,10,64,64,1);
  assert(wl_display_roundtrip(display)>=0 && failed==2 && last_reason==MATON_AHB_SESSION_V1_FAILURE_QUOTA);
  wl_surface_attach(surface,buffers[0],0,0);wl_surface_damage(surface,0,0,64,64);wl_surface_commit(surface);
  assert(wl_display_roundtrip(display)>=0 && !releases);
  wl_surface_attach(surface,NULL,0,0);wl_surface_commit(surface);
  assert(wl_display_roundtrip(display)>=0 && !releases);
  /* Next event is produced only after the server's held reader unlocks. */
  while(!releases)assert(wl_display_dispatch(display)>=0);
  assert(releases==1);
  close(transport);
  struct maton_ahb_session_v1* broken=maton_ahb_manager_v1_get_session(manager,surface);
  maton_ahb_session_v1_add_listener(broken,&session_listener,NULL);
  assert(wl_display_roundtrip(display)>=0);close(transport);transport=-1;
  maton_ahb_session_v1_create_buffer(broken,1,64,64,1);
  assert(wl_display_roundtrip(display)>=0 && failed==3 && last_reason==MATON_AHB_SESSION_V1_FAILURE_TRANSPORT);
  maton_ahb_session_v1_destroy(broken);
  wl_surface_destroy(surface);
  maton_ahb_session_v1_create_buffer(session,11,64,64,1);
  assert(wl_display_roundtrip(display)>=0 && failed==4 && last_reason==MATON_AHB_SESSION_V1_FAILURE_SURFACE_LOST);
  maton_ahb_session_v1_destroy(session);
  for(unsigned i=0;i<8;i++)wl_buffer_destroy(buffers[i]);
  maton_ahb_manager_v1_destroy(manager);wl_compositor_destroy(compositor);wl_registry_destroy(registry);
  assert(wl_display_roundtrip(display)>=0);wl_display_disconnect(display);
  assert(!kill(pid,SIGTERM));int status;assert(waitpid(pid,&status,0)==pid);
#ifdef MATON_AHB_LINUX_CLIENT
  struct mv_scope scope;assert(switch_to_android(adoption,fast,&scope));
  destroy_device(driver.device,NULL);destroy_instance(vk_instance,NULL);switch_back(&scope);
  puts("PASS resident TLS: Linux Wayland client received and imported 8 AHBs into resident Vulkan images");
#endif
  puts("PASS AHB protocol: separate processes, 8 complete handle transfers, typed scene source, delayed reader release, quotas, invalid extent, closed transport/SIGPIPE and surface loss");
  return 0;
}
