#define _GNU_SOURCE
/* Real surface/swapchain code, simulated Android driver + X server. No GPU
 * is required; protocol ownership and release timing are checked explicitly. */
#include "../icd.c"
#include "../wsi_x11.c"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
_Static_assert(sizeof(struct mv_msghdr)==sizeof(struct msghdr),"Linux sendmsg ABI");
_Static_assert(sizeof(struct mv_cmsghdr)==sizeof(struct cmsghdr),"Linux SCM_RIGHTS ABI");
_Static_assert(sizeof(struct mv_sockaddr_un)==sizeof(struct sockaddr_un),"Linux Unix socket ABI");
static int allow_idle,allow_complete,imports,presents,queue_token,refuse_import;
static uint32_t ids=100,last_pixmap,last_serial,event_id;
static struct cookie cookie={1};
static uint32_t xid(void *c){assert(c==&queue_token);return ++ids;}
static int yes(void *c){assert(c==&queue_token);return 1;}
static int no(void *c){assert(c==&queue_token);return 0;}
static struct cookie geometry_request(void *c,uint32_t w){assert(c==&queue_token&&w==42);return cookie;}
static struct geometry *geometry_result(void *c,struct cookie ck,void **error){
    (void)c;(void)ck;(void)error;struct geometry *g=calloc(1,sizeof(*g));g->depth=24;g->width=320;g->height=200;return g;
}
static void *request_check(void *c,struct cookie ck){(void)c;(void)ck;return 0;}
static char socket_root[]="/tmp/maton-x11-ahb-XXXXXX",socket_directory[512],socket_path[512];
static int private_listener,private_directory_fd;
static uint32_t import_reply;
static int private_socket(int domain,int type,int protocol) {
    assert(domain==1&&type==(5|02000000)&&!protocol);return socket(domain,type,protocol);
}
static int connect_import(int fd,const struct mv_sockaddr *addr,uint32_t size) {
    const struct mv_sockaddr_un *un=(const void *)addr;
    assert(fd>=0&&size==sizeof(*un)&&!strncmp(un->sun_path,"/proc/self/fd/",14));
    return connect(fd,(const struct sockaddr *)addr,size);
}
static int close_import(int fd) {return close(fd);}
static int open_import(const char *path,int flags,...) {
    assert(!strcmp(path,socket_directory)&&flags==(O_PATH|O_DIRECTORY|O_CLOEXEC));return open(path,flags);
}
static ssize_t import_ahb(int fd,const struct mv_msghdr *msg,int flags) {
    assert(fd>=0&&flags==0x4000&&msg->msg_iovlen==1&&msg->msg_iov[0].iov_len==20);
    /* Exercise a real kernel SCM_RIGHTS transfer using the ICD's libc ABI. */
    int pair[2];assert(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)==0);
    assert(sendmsg(pair[0],(const struct msghdr *)msg,flags)==20);
    uint32_t request[5];struct iovec iov={request,sizeof(request)};
    union {struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int))];} rights;
    struct msghdr received={.msg_iov=&iov,.msg_iovlen=1,.msg_control=rights.bytes,.msg_controllen=sizeof(rights.bytes)};
    assert(recvmsg(pair[1],&received,MSG_CMSG_CLOEXEC)==20);
    const uint32_t *r=request;
    assert(r[0]==320&&r[1]==200&&r[2]==5&&r[3]==42&&r[4]>100);
    struct cmsghdr *cm=CMSG_FIRSTHDR(&received);int transport;
    assert(cm&&cm->cmsg_level==1&&cm->cmsg_type==1&&cm->cmsg_len==MV_CMSG_LEN(sizeof(int)));
    memcpy(&transport,MV_CMSG_DATA(cm),sizeof(transport));
    char marker;assert(read(transport,&marker,1)==1&&marker=='A');
    close(transport);close(pair[0]);close(pair[1]);
    imports++;import_reply=refuse_import?0:r[4];return 20;
}
static ssize_t import_result(int fd,void *data,size_t size,int flags) {
    assert(fd>=0&&size==4&&!flags);memcpy(data,&import_reply,4);return 4;
}
static char *private_env(const char *name) {assert(!strcmp(name,"MATON_X11_AHB_SOCKET"));return socket_path;}
static struct cookie release_pixmap(void *c,uint32_t pixmap){(void)c;assert(pixmap>100);return cookie;}
static struct cookie select_events(void *c,uint32_t id,uint32_t window,uint32_t mask){
    (void)c;assert(window==42&&(mask==7||mask==0));event_id=id;return cookie;
}
static void *register_events(void *c,void *extension,uint32_t id,uint32_t *stamp){
    (void)c;(void)extension;(void)id;(void)stamp;return &queue_token;
}
static void unregister_events(void *c,void *queue){(void)c;assert(queue==&queue_token);}
static struct idle *next_event(void *c,void *queue){
    (void)c;(void)queue;
    if(allow_complete){allow_complete=0;struct idle *e=calloc(1,44);e->event_type=1;e->event=event_id;e->window=42;e->serial=last_serial;
        uint64_t msc=50;memcpy((unsigned char *)e+36,&msc,8);return e;}
    if(!allow_idle)return 0;allow_idle=0;
    struct idle *e=calloc(1,sizeof(*e));e->event_type=2;e->event=event_id;e->window=42;e->pixmap=last_pixmap;e->serial=last_serial;return e;
}
static struct cookie show(void *c,uint32_t window,uint32_t pixmap,uint32_t serial,uint32_t valid,uint32_t update,
        int16_t x,int16_t y,uint32_t crtc,uint32_t wait,uint32_t idle,uint32_t options,
        uint64_t target,uint64_t divisor,uint64_t remainder,uint32_t count,const void *notifies){
    (void)c;assert(window==42&&pixmap>100&&!valid&&!update&&!x&&!y&&!crtc&&!wait&&!idle&&!options&&(target==0||target==51)&&!divisor&&!remainder&&!count&&!notifies);
    last_pixmap=pixmap;last_serial=serial;presents++;return cookie;
}
static unsigned char setup_data[40],depth_data=24;
static struct visual visual_data={.id=7,.kind=4,.red=0xff0000,.green=0xff00,.blue=0xff};
static unsigned char extension_reply[32]={[8]=1};
static const unsigned char *extension(void *c,void *id){(void)c;(void)id;return extension_reply;}
static const unsigned char *get_setup(void *c){(void)c;return setup_data;}
static struct iterator root_iter(const void *s){(void)s;return (struct iterator){&queue_token,1,0};}
static struct iterator depth_iter(const void *s){(void)s;return (struct iterator){&depth_data,1,0};}
static struct iterator visual_iter(const void *s){(void)s;return (struct iterator){&visual_data,1,0};}
static void iter_next(struct iterator *it){it->rem--;}
static struct attributes *attrs_result(void *c,struct cookie ck,void **err){
    (void)c;(void)ck;(void)err;struct attributes *a=calloc(1,sizeof(*a));a->visual=7;return a;
}
static void *xlib_conn(void *display){assert(display==&queue_token);return display;}
void *mv_client_library(const char *name){(void)name;return &queue_token;}
void *mv_client_symbol(void *lib,const char *name){assert(lib==&queue_token);
#define SYM(n,v) if(!strcmp(name,n))return (void *)(v)
    SYM("XGetXCBConnection",xlib_conn);SYM("xcb_get_extension_data",extension);
    SYM("xcb_get_setup",get_setup);SYM("xcb_setup_roots_iterator",root_iter);
    SYM("xcb_screen_allowed_depths_iterator",depth_iter);SYM("xcb_depth_visuals_iterator",visual_iter);
    SYM("xcb_screen_next",iter_next);SYM("xcb_depth_next",iter_next);SYM("xcb_visualtype_next",iter_next);
    SYM("xcb_get_window_attributes",geometry_request);SYM("xcb_get_window_attributes_reply",attrs_result);
    SYM("free",free);SYM("open",open_import);SYM("snprintf",snprintf);SYM("getenv",private_env);SYM("socket",private_socket);SYM("connect",connect_import);
    SYM("close",close_import);SYM("sendmsg",import_ahb);SYM("recv",import_result);SYM("xcb_generate_id",xid);SYM("xcb_flush",yes);
    SYM("xcb_connection_has_error",no);SYM("xcb_get_file_descriptor",no);
    SYM("xcb_get_geometry",geometry_request);SYM("xcb_get_geometry_reply",geometry_result);
    SYM("xcb_request_check",request_check);SYM("xcb_free_pixmap",release_pixmap);
    SYM("xcb_register_for_special_xge",register_events);SYM("xcb_unregister_for_special_event",unregister_events);
    SYM("xcb_poll_for_special_event",next_event);SYM("xcb_wait_for_special_event",next_event);
    SYM("xcb_present_select_input_checked",select_events);
    SYM("xcb_present_pixmap_checked",show);SYM("xcb_present_id",&queue_token);
#undef SYM
    return 0;
}
/* Wayland is deliberately absent: an XCB-only process must not load it. */
void *mv_wayland_symbol(const char *name){(void)name;return 0;}
static struct adopt_thread thread;
static struct adopt_thread *current(void){thread.resident_fs=mv_tp_read(0);return &thread;}
/* AHB is opaque; no gralloc layout or native handle is exposed to the WSI. */
static int ahb_allocate(const void *d,struct AHardwareBuffer **out){const struct mv_ahb_desc *desc=d;
    assert(desc->format==5&&desc->width==320&&desc->height==200);*out=malloc(1);return 0;}
static void ahb_describe(const struct AHardwareBuffer *a,void *out){(void)a;*(struct mv_ahb_desc *)out=(struct mv_ahb_desc){.width=320,.height=200,.layers=1,.format=5,.stride=320};}
static int released_ahbs;
static void ahb_release(struct AHardwareBuffer *a){free(a);released_ahbs++;}
static int ahb_send(const struct AHardwareBuffer *a,int fd){assert(a);return write(fd,"A",1)==1?0:-1;}
static VkResult ahb_properties(VkDevice d,const struct AHardwareBuffer *a,VkAndroidHardwareBufferPropertiesANDROID *p){(void)d;(void)a;
    VkAndroidHardwareBufferFormatPropertiesANDROID *f=p->pNext;f->format=VK_FORMAT_B8G8R8A8_UNORM;f->formatFeatures=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    p->memoryTypeBits=1;p->allocationSize=256000;return VK_SUCCESS;}
static VkResult create_image(VkDevice d,const VkImageCreateInfo *info,const VkAllocationCallbacks *a,VkImage *out){
    (void)d;(void)a;assert(info->format==VK_FORMAT_B8G8R8A8_UNORM&&info->tiling==VK_IMAGE_TILING_OPTIMAL);
    const VkExternalMemoryImageCreateInfo *ext=info->pNext;assert(ext->handleTypes==VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
    *out=(VkImage)(uintptr_t)++ids;return VK_SUCCESS;
}
static VkResult allocate_memory(VkDevice d,const VkMemoryAllocateInfo *a,const VkAllocationCallbacks *b,VkDeviceMemory *out){(void)d;(void)a;(void)b;*out=(VkDeviceMemory)(uintptr_t)++ids;return VK_SUCCESS;}
static VkResult bind_image(VkDevice d,VkImage i,VkDeviceMemory m,VkDeviceSize o){(void)d;(void)i;(void)m;(void)o;return VK_SUCCESS;}
static void destroy_image(VkDevice d,VkImage i,const VkAllocationCallbacks *a){(void)d;(void)i;(void)a;}
static void free_memory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks *a){(void)d;(void)m;(void)a;}
static PFN_vkVoidFunction gdpa(VkDevice d,const char *name){(void)d;
#define SYM(n,v) if(!strcmp(name,n))return (PFN_vkVoidFunction)(v)
    SYM("vkCreateImage",create_image);SYM("vkAllocateMemory",allocate_memory);SYM("vkBindImageMemory",bind_image);
    SYM("vkDestroyImage",destroy_image);SYM("vkFreeMemory",free_memory);SYM("vkGetAndroidHardwareBufferPropertiesANDROID",ahb_properties);
#undef SYM
    return 0;
}
int main(void){
    assert(mkdtemp(socket_root));
    snprintf(socket_directory,sizeof(socket_directory),"%s/%0120d",socket_root,0);
    assert(mkdir(socket_directory,0700)==0);
    assert(snprintf(socket_path,sizeof(socket_path),"%s/X0.ahb",socket_directory)<(int)sizeof(socket_path));
    assert(strlen(socket_path)>sizeof(((struct sockaddr_un *)0)->sun_path));
    private_directory_fd=open(socket_directory,O_PATH|O_DIRECTORY|O_CLOEXEC);assert(private_directory_fd>=0);
    struct sockaddr_un address={.sun_family=AF_UNIX};
    snprintf(address.sun_path,sizeof(address.sun_path),"/proc/self/fd/%d/X0.ahb",private_directory_fd);
    private_listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);assert(private_listener>=0);
    assert(bind(private_listener,(const struct sockaddr *)&address,sizeof(address))==0&&listen(private_listener,8)==0);
    struct adopt_api api={.version=1,.current=current};mv_adopt=&api;
    mv_ahb_allocate=ahb_allocate;mv_ahb_send_socket=ahb_send;mv_ahb_describe=ahb_describe;mv_ahb_release=ahb_release;
    assert(mv_x11_visual_supported(&queue_token,7));
    assert(!mv_x11_visual_supported(&queue_token,8));
    extension_reply[8]=0;assert(!mv_x11_visual_supported(&queue_token,7));extension_reply[8]=1;
    setup_data[30]=1;assert(!mv_x11_visual_supported(&queue_token,7));setup_data[30]=0;
    depth_data=30;assert(!mv_x11_visual_supported(&queue_token,7));depth_data=24;
    depth_data=32;assert(mv_x11_visual_supported(&queue_token,7));depth_data=24;
    struct mv_instance instance={.wsi_surface_enabled=1,.wsi_xcb_enabled=1};
    VkXcbSurfaceCreateInfoKHR info={.sType=VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,.connection=(void *)&queue_token,.window=42};
    VkSurfaceKHR surface;assert(mv_vkCreateXcbSurfaceKHR((VkInstance)&instance,&info,0,&surface)==VK_SUCCESS);
    struct mv_handle physical={.instance=&instance};VkSurfaceCapabilitiesKHR caps;
    assert(mv_vkGetPhysicalDeviceSurfaceCapabilitiesKHR((VkPhysicalDevice)&physical,surface,&caps)==VK_SUCCESS);
    assert(caps.currentExtent.width==320&&caps.minImageExtent.width==320&&caps.maxImageExtent.height==200);
    struct mv_device device={.wsi_swapchain_enabled=1,.gdpa=gdpa};device.base.instance=&instance;
    VkSwapchainCreateInfoKHR ci={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=surface,.minImageCount=2,
        .imageFormat=VK_FORMAT_B8G8R8A8_UNORM,.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,.imageExtent={320,200},
        .imageArrayLayers=1,.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,
        .preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,.presentMode=VK_PRESENT_MODE_FIFO_KHR};
    VkSwapchainKHR swapchain;assert(mv_vkCreateSwapchainKHR((VkDevice)&device,&ci,0,&swapchain)==VK_SUCCESS&&imports==2);
    /* Import failure must unwind AHB ownership without submitting a pixmap. */
    refuse_import=1;VkSwapchainKHR refused;
    assert(mv_vkCreateSwapchainKHR((VkDevice)&device,&ci,0,&refused)!=VK_SUCCESS&&imports==3);
    refuse_import=0;
    struct mv_wsi_swapchain *chain=(void *)(uintptr_t)swapchain;uint32_t index;
    assert(mv_wsi_acquire(chain,&index)==VK_SUCCESS);
    assert(mv_wsi_present(chain,index,-1)==VK_SUCCESS&&presents==1);
    assert(mv_x11_dispatch(((struct mv_wsi_live_context *)chain->ops.context)->surface->x11,0)==0);
    mv_wsi_collect_releases(chain);assert(chain->images[index].state==2);
    allow_idle=1;assert(mv_x11_dispatch(((struct mv_wsi_live_context *)chain->ops.context)->surface->x11,0)==1);
    mv_wsi_collect_releases(chain);assert(chain->images[index].state==0);
    allow_complete=1;assert(mv_wsi_acquire(chain,&index)==VK_SUCCESS);assert(mv_wsi_present(chain,index,-1)==VK_SUCCESS);
    /* Last displayed pixmap never goes idle; destruction must not wait. */
    mv_vkDestroySwapchainKHR((VkDevice)&device,swapchain,0);assert(released_ahbs==3);
    mv_vkDestroySurfaceKHR((VkInstance)&instance,surface,0);
    instance.wsi_xlib_enabled=1;
    VkXlibSurfaceCreateInfoKHR xi={.sType=VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,.dpy=(void *)&queue_token,.window=42};
    assert(mv_vkCreateXlibSurfaceKHR((VkInstance)&instance,&xi,0,&surface)==VK_SUCCESS);
    mv_vkDestroySurfaceKHR((VkInstance)&instance,surface,0);
    close(private_listener);close(private_directory_fd);
    assert(unlink(socket_path)==0&&rmdir(socket_directory)==0&&rmdir(socket_root)==0);
    puts("PASS XCB Vulkan surface/swapchain: opaque RGBA AHB transport, long socket paths, import rejection, Present idle, teardown without final idle");
}
