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
static int source_fd,imported_fd,allow_idle,allow_complete,imports,presents,queue_token;
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
static struct cookie import_buffer(void *c,uint32_t pixmap,uint32_t drawable,uint32_t size,
        uint16_t width,uint16_t height,uint16_t stride,uint8_t depth,uint8_t bpp,int32_t fd){
    assert(c==&queue_token&&drawable==42&&size==256000&&width==320&&height==200&&stride==1280&&depth==24&&bpp==32);
    assert(fd!=source_fd&&fcntl(fd,F_GETFD)>=0&&fcntl(source_fd,F_GETFD)>=0);
    imported_fd=fd;close(fd);imports++;last_pixmap=pixmap;return cookie;
}
static struct cookie release_pixmap(void *c,uint32_t pixmap){(void)c;assert(pixmap>100);return cookie;}
static struct cookie select_events(void *c,uint32_t id,uint32_t window,uint32_t mask){
    (void)c;assert(window==42&&(mask==6||mask==0));event_id=id;return cookie;
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
    SYM("XGetXCBConnection",xlib_conn);SYM("xcb_get_extension_data",extension);SYM("xcb_dri3_id",&queue_token);
    SYM("xcb_get_setup",get_setup);SYM("xcb_setup_roots_iterator",root_iter);
    SYM("xcb_screen_allowed_depths_iterator",depth_iter);SYM("xcb_depth_visuals_iterator",visual_iter);
    SYM("xcb_screen_next",iter_next);SYM("xcb_depth_next",iter_next);SYM("xcb_visualtype_next",iter_next);
    SYM("xcb_get_window_attributes",geometry_request);SYM("xcb_get_window_attributes_reply",attrs_result);
    SYM("free",free);SYM("dup",dup);SYM("xcb_generate_id",xid);SYM("xcb_flush",yes);
    SYM("xcb_connection_has_error",no);SYM("xcb_get_file_descriptor",no);
    SYM("xcb_get_geometry",geometry_request);SYM("xcb_get_geometry_reply",geometry_result);
    SYM("xcb_request_check",request_check);SYM("xcb_free_pixmap",release_pixmap);
    SYM("xcb_register_for_special_xge",register_events);SYM("xcb_unregister_for_special_event",unregister_events);
    SYM("xcb_poll_for_special_event",next_event);SYM("xcb_wait_for_special_event",next_event);
    SYM("xcb_dri3_pixmap_from_buffer_checked",import_buffer);SYM("xcb_present_select_input_checked",select_events);
    SYM("xcb_present_pixmap_checked",show);SYM("xcb_present_id",&queue_token);
#undef SYM
    return 0;
}
/* Wayland is deliberately absent: an XCB-only process must not load it. */
void *mv_wayland_symbol(const char *name){(void)name;return 0;}
static struct adopt_thread thread;
static struct adopt_thread *current(void){thread.resident_fs=mv_tp_read(0);return &thread;}
static void requirements(VkDevice d,VkImage i,VkMemoryRequirements *r){(void)d;(void)i;*r=(VkMemoryRequirements){.size=256000,.alignment=4096,.memoryTypeBits=1};}
static VkResult memory_fd(VkDevice d,const VkMemoryGetFdInfoKHR *info,int *fd){(void)d;
    assert(info->handleType==VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);*fd=dup(source_fd);return *fd<0?VK_ERROR_OUT_OF_HOST_MEMORY:VK_SUCCESS;}
static VkResult create_image(VkDevice d,const VkImageCreateInfo *info,const VkAllocationCallbacks *a,VkImage *out){
    (void)d;(void)a;assert(info->format==VK_FORMAT_B8G8R8A8_UNORM&&info->tiling==VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT);
    const VkExternalMemoryImageCreateInfo *ext=info->pNext;assert(ext->handleTypes==VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
    const VkImageDrmFormatModifierListCreateInfoEXT *mod=ext->pNext;assert(mod->drmFormatModifierCount==1&&mod->pDrmFormatModifiers[0]==0);*out=(VkImage)(uintptr_t)++ids;return VK_SUCCESS;
}
static VkResult allocate_memory(VkDevice d,const VkMemoryAllocateInfo *a,const VkAllocationCallbacks *b,VkDeviceMemory *out){(void)d;(void)a;(void)b;*out=(VkDeviceMemory)(uintptr_t)++ids;return VK_SUCCESS;}
static VkResult bind_image(VkDevice d,VkImage i,VkDeviceMemory m,VkDeviceSize o){(void)d;(void)i;(void)m;(void)o;return VK_SUCCESS;}
static void destroy_image(VkDevice d,VkImage i,const VkAllocationCallbacks *a){(void)d;(void)i;(void)a;}
static void free_memory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks *a){(void)d;(void)m;(void)a;}
static void layout(VkDevice d,VkImage i,const VkImageSubresource *s,VkSubresourceLayout *l){(void)d;(void)i;assert(s->aspectMask==VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT);l->rowPitch=1280;}
static PFN_vkVoidFunction gdpa(VkDevice d,const char *name){(void)d;
#define SYM(n,v) if(!strcmp(name,n))return (PFN_vkVoidFunction)(v)
    SYM("vkCreateImage",create_image);SYM("vkAllocateMemory",allocate_memory);SYM("vkBindImageMemory",bind_image);
    SYM("vkDestroyImage",destroy_image);SYM("vkFreeMemory",free_memory);SYM("vkGetImageMemoryRequirements",requirements);SYM("vkGetMemoryFdKHR",memory_fd);
    SYM("vkGetImageSubresourceLayout",layout);
#undef SYM
    return 0;
}
int main(void){
    source_fd=open("/dev/null",O_RDONLY);assert(source_fd>=0);
    struct adopt_api api={.version=1,.current=current};mv_adopt=&api;
    assert(mv_x11_visual_supported(&queue_token,7));
    assert(!mv_x11_visual_supported(&queue_token,8));
    extension_reply[8]=0;assert(!mv_x11_visual_supported(&queue_token,7));extension_reply[8]=1;
    setup_data[30]=1;assert(!mv_x11_visual_supported(&queue_token,7));setup_data[30]=0;
    depth_data=30;assert(!mv_x11_visual_supported(&queue_token,7));depth_data=24;
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
    assert(fcntl(source_fd,F_GETFD)>=0&&fcntl(imported_fd,F_GETFD)==-1);
    struct mv_wsi_swapchain *chain=(void *)(uintptr_t)swapchain;uint32_t index;
    assert(mv_wsi_acquire(chain,&index)==VK_SUCCESS);
    assert(mv_wsi_present(chain,index,-1)==VK_SUCCESS&&presents==1);
    assert(mv_x11_dispatch(((struct mv_wsi_live_context *)chain->ops.context)->surface->x11,0)==0);
    mv_wsi_collect_releases(chain);assert(chain->images[index].state==2);
    allow_idle=1;assert(mv_x11_dispatch(((struct mv_wsi_live_context *)chain->ops.context)->surface->x11,0)==1);
    mv_wsi_collect_releases(chain);assert(chain->images[index].state==0);
    allow_complete=1;assert(mv_wsi_acquire(chain,&index)==VK_SUCCESS);assert(mv_wsi_present(chain,index,-1)==VK_SUCCESS);
    /* Last displayed pixmap never goes idle; destruction must not wait. */
    mv_vkDestroySwapchainKHR((VkDevice)&device,swapchain,0);assert(fcntl(source_fd,F_GETFD)>=0);
    mv_vkDestroySurfaceKHR((VkInstance)&instance,surface,0);
    instance.wsi_xlib_enabled=1;
    VkXlibSurfaceCreateInfoKHR xi={.sType=VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,.dpy=(void *)&queue_token,.window=42};
    assert(mv_vkCreateXlibSurfaceKHR((VkInstance)&instance,&xi,0,&surface)==VK_SUCCESS);
    mv_vkDestroySurfaceKHR((VkInstance)&instance,surface,0);close(source_fd);
    puts("PASS XCB Vulkan surface/swapchain: BGRA import, fd ownership, Present idle, teardown without final idle");
}
