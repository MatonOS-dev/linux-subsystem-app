/* Loader-facing Wayland WSI commands, included from icd.c.
 *
 * The ICD links no libwayland and owns no wl_display/wl_surface. A Linux-side
 * helper (linked into the payload, on the application's own TLS) supplies the
 * transport session, its AHB allocator and frame pacing through mv_wsi_helper;
 * the enclosing translation unit supplies resident import/format queries.
 * Everything here is gated on MV_WSI_PROTOTYPE, which the default build leaves
 * undefined, so a stock DSO exposes no WSI name, extension or entry point. */
#ifndef MV_WSI_PROTOTYPE
#define MV_WSI_PROTOTYPE 0
#endif
#include "wsi.h"
#include "runtime.h"
#include <vulkan/vulkan_wayland.h>
/* Provided by the enclosing ICD (icd.c) or a host test harness. */
static const struct mv_wsi_helper *mv_wsi_bootstrap_helper(void);
#if MV_WSI_PROTOTYPE
static VkResult mv_wsi_bridged_query(VkPhysicalDevice,const VkPhysicalDeviceImageFormatInfo2 *,
    VkImageFormatProperties2 *);
static VkResult mv_wsi_resident_import(VkDevice,struct AHardwareBuffer *,VkExtent2D,VkFormat,
    VkImage *,VkDeviceMemory *);
static void mv_wsi_resident_release(VkDevice,VkImage,VkDeviceMemory);
#endif
struct mv_wsi_surface_object { struct mv_wsi_surface model; void *instance; VkPhysicalDevice physical; };
struct mv_wsi_swapchain_object { struct mv_wsi_swapchain *model; struct mv_wsi_surface_object *surface;
    VkDevice device; const struct mv_wsi_ops *helper_ops; struct mv_wsi_ops ops; };
/* Private instance extensions the helper makes real. Version 1 is the only
 * wire revision we speak; VK_KHR_get_surface_capabilities2 backs the 2 variants. */
static const char *const mv_wsi_instance_names[]={"VK_KHR_surface","VK_KHR_wayland_surface",
    "VK_KHR_get_surface_capabilities2"};
static const char *const mv_wsi_device_names[]={"VK_KHR_swapchain"};
static int mv_wsi_instance_enabled(void) { return MV_WSI_PROTOTYPE && mv_wsi_bootstrap_helper()!=0; }
static int mv_wsi_instance_extension(const char *name) {
    if(!mv_wsi_instance_enabled())return 0;
    for(unsigned j=0;j<sizeof(mv_wsi_instance_names)/sizeof(*mv_wsi_instance_names);j++)
        if(!strcmp(name,mv_wsi_instance_names[j]))return 1;
    return 0;
}
static int mv_wsi_device_extension(const char *name) {
    if(!mv_wsi_instance_enabled())return 0;
    for(unsigned j=0;j<sizeof(mv_wsi_device_names)/sizeof(*mv_wsi_device_names);j++)
        if(!strcmp(name,mv_wsi_device_names[j]))return 1;
    return 0;
}
static VkResult mv_wsi_enumerate_instance_extensions(uint32_t *n,VkExtensionProperties *out) {
    uint32_t count=sizeof(mv_wsi_instance_names)/sizeof(*mv_wsi_instance_names);
    if(!mv_wsi_instance_enabled()){*n=0;return VK_SUCCESS;}
    if(!out){*n=count;return VK_SUCCESS;}
    uint32_t capacity=*n,written=0;
    for(uint32_t j=0;j<count;j++){
        if(written<capacity){VkExtensionProperties *p=&out[written];memset(p,0,sizeof(*p));
            for(unsigned k=0;k<sizeof(p->extensionName)-1 && mv_wsi_instance_names[j][k];k++)p->extensionName[k]=mv_wsi_instance_names[j][k];
            p->specVersion=1;}
        written++;
    }
    *n=written>capacity?capacity:written;
    return written>capacity?VK_INCOMPLETE:VK_SUCCESS;
}
static uint32_t mv_wsi_device_extension_count(void) {
    return mv_wsi_instance_enabled()?(uint32_t)(sizeof(mv_wsi_device_names)/sizeof(*mv_wsi_device_names)):0;
}
static void mv_wsi_device_extension_at(uint32_t index,VkExtensionProperties *p) {
    memset(p,0,sizeof(*p));
    for(unsigned k=0;k<sizeof(p->extensionName)-1;k++){
        p->extensionName[k]=mv_wsi_device_names[index][k];
        if(!mv_wsi_device_names[index][k])break;
    }
    p->specVersion=1;
}
#if MV_WSI_PROTOTYPE
/* Surface queries need negotiated limits, which only exist after the resident
 * driver answered for this physical device. Attach lazily and cache it. */
static VkResult mv_wsi_surface_ready(struct mv_wsi_surface_object *s,VkPhysicalDevice physical) {
    if(!s->model.alive)return VK_ERROR_SURFACE_LOST_KHR;
    if(s->physical==physical && s->model.limits.format_count)return VK_SUCCESS;
    VkResult r=mv_wsi_surface_attach_device(&s->model,physical,mv_wsi_bridged_query);
    if(r)return r;
    s->physical=physical;
    return VK_SUCCESS;
}
/* The transport session has no configured extent of its own here: report the
 * variable-extent sentinel so the client chooses, as a Wayland client does. */
static VkResult mv_wsi_surface_capabilities(struct mv_wsi_surface_object *s,VkSurfaceCapabilitiesKHR *caps) {
    VkExtent2D saved=s->model.extent;
    int variable=!saved.width||!saved.height;
    if(variable)s->model.extent=s->model.limits.max_extent;
    VkResult r=mv_wsi_capabilities(&s->model,caps);
    s->model.extent=saved;
    if(r)return r;
    if(variable)caps->currentExtent=(VkExtent2D){UINT32_MAX,UINT32_MAX};
    return VK_SUCCESS;
}
static VkResult mv_wsi_bind_allocate(void *ctx,uint64_t surface,VkExtent2D e,VkFormat f,
    struct AHardwareBuffer **b,uint64_t *token) {
    struct mv_wsi_swapchain_object *sc=ctx;
    if(!sc->helper_ops || !sc->helper_ops->allocate)return VK_ERROR_INITIALIZATION_FAILED;
    return sc->helper_ops->allocate(sc->helper_ops->context,surface,e,f,b,token);
}
static void mv_wsi_bind_release(void *ctx,struct AHardwareBuffer *b,uint64_t token) {
    struct mv_wsi_swapchain_object *sc=ctx;
    if(sc->helper_ops && sc->helper_ops->release)sc->helper_ops->release(sc->helper_ops->context,b,token);
}
static VkResult mv_wsi_bind_import(void *ctx,struct AHardwareBuffer *b,VkExtent2D e,VkFormat f,
    VkImage *image,VkDeviceMemory *memory) {
    struct mv_wsi_swapchain_object *sc=ctx;
    return mv_wsi_resident_import(sc->device,b,e,f,image,memory);
}
static void mv_wsi_bind_destroy(void *ctx,VkImage image,VkDeviceMemory memory) {
    struct mv_wsi_swapchain_object *sc=ctx;
    mv_wsi_resident_release(sc->device,image,memory);
}
static VkResult mv_wsi_bind_present(void *ctx,uint64_t surface,uint64_t buffer,uint64_t serial) {
    struct mv_wsi_swapchain_object *sc=ctx;
    if(!sc->helper_ops || !sc->helper_ops->present)return VK_ERROR_INITIALIZATION_FAILED;
    return sc->helper_ops->present(sc->helper_ops->context,surface,buffer,serial);
}
static VkResult mv_wsi_bind_pace(void *ctx,uint64_t surface,uint64_t timeout_ns) {
    struct mv_wsi_swapchain_object *sc=ctx;
    if(!sc->helper_ops || !sc->helper_ops->pace)return VK_ERROR_INITIALIZATION_FAILED;
    return sc->helper_ops->pace(sc->helper_ops->context,surface,timeout_ns);
}
static VkResult mv_vkCreateWaylandSurfaceKHR(VkInstance instance,
    const VkWaylandSurfaceCreateInfoKHR *info,const VkAllocationCallbacks *a,VkSurfaceKHR *out) {
    (void)a;*out=VK_NULL_HANDLE;
    const struct mv_wsi_helper *h=mv_wsi_bootstrap_helper();
    if(!h)return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(!info || info->sType!=VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR || info->pNext || info->flags ||
       !info->display || !info->surface)return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_wsi_surface_object *s=mv_alloc(sizeof(*s));
    if(!s)return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(s,0,sizeof(*s));
    VkResult r=mv_wsi_surface_negotiate(&s->model,h,info->display,info->surface);
    if(r){mv_free(s);return r;}
    s->instance=(void *)instance;
    *out=(VkSurfaceKHR)s;
    return VK_SUCCESS;
}
static void mv_vkDestroySurfaceKHR(VkInstance instance,VkSurfaceKHR surface,const VkAllocationCallbacks *a) {
    (void)instance;(void)a;
    if(!surface)return;
    struct mv_wsi_surface_object *s=(void *)surface;
    mv_wsi_surface_retire(&s->model);
    mv_free(s);
}
static VkResult mv_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physical,uint32_t family,
    VkSurfaceKHR surface,VkBool32 *supported) {
    (void)family;*supported=VK_FALSE;
    if(!surface)return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_wsi_surface_object *s=(void *)surface;
    VkResult r=mv_wsi_surface_ready(s,physical);
    if(r==VK_ERROR_SURFACE_LOST_KHR)return r;
    *supported=mv_wsi_support(&s->model,VK_QUEUE_GRAPHICS_BIT,1);
    return VK_SUCCESS;
}
static VkResult mv_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physical,
    VkSurfaceKHR surface,VkSurfaceCapabilitiesKHR *caps) {
    if(!surface || !caps)return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_wsi_surface_object *s=(void *)surface;
    VkResult r=mv_wsi_surface_ready(s,physical);
    if(r)return r;
    return mv_wsi_surface_capabilities(s,caps);
}
static VkResult mv_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physical,VkSurfaceKHR surface,
    uint32_t *n,VkSurfaceFormatKHR *formats) {
    if(!n)return VK_ERROR_INITIALIZATION_FAILED;
    if(!surface){*n=0;return VK_ERROR_INITIALIZATION_FAILED;}
    struct mv_wsi_surface_object *s=(void *)surface;
    VkResult r=mv_wsi_surface_ready(s,physical);
    if(r){*n=0;return r;}
    return mv_wsi_formats(&s->model,n,formats);
}
static VkResult mv_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physical,VkSurfaceKHR surface,
    uint32_t *n,VkPresentModeKHR *modes) {
    if(!n)return VK_ERROR_INITIALIZATION_FAILED;
    if(!surface){*n=0;return VK_ERROR_INITIALIZATION_FAILED;}
    struct mv_wsi_surface_object *s=(void *)surface;
    VkResult r=mv_wsi_surface_ready(s,physical);
    if(r){*n=0;return r;}
    return mv_wsi_modes(n,modes);
}
static VkBool32 mv_vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physical,
    uint32_t family,struct wl_display *display) {
    (void)physical;(void)family;(void)display;
    return mv_wsi_bootstrap_helper()?VK_TRUE:VK_FALSE;
}
static VkResult mv_vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physical,
    const VkPhysicalDeviceSurfaceInfo2KHR *info,VkSurfaceCapabilities2KHR *caps) {
    if(!info || !caps)return VK_ERROR_INITIALIZATION_FAILED;
    return mv_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,info->surface,&caps->surfaceCapabilities);
}
static VkResult mv_vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physical,
    const VkPhysicalDeviceSurfaceInfo2KHR *info,uint32_t *n,VkSurfaceFormat2KHR *formats) {
    if(!info || !n)return VK_ERROR_INITIALIZATION_FAILED;
    if(!formats){
        uint32_t count=0;
        VkResult r=mv_vkGetPhysicalDeviceSurfaceFormatsKHR(physical,info->surface,&count,0);
        *n=r==VK_SUCCESS?count:0;
        return r;
    }
    uint32_t capacity=*n;
    VkSurfaceFormatKHR *tmp=capacity?mv_alloc((size_t)capacity*sizeof(*tmp)):0;
    if(capacity && !tmp)return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t count=capacity;
    VkResult r=mv_vkGetPhysicalDeviceSurfaceFormatsKHR(physical,info->surface,&count,tmp);
    if(!r || r==VK_INCOMPLETE)
        for(uint32_t j=0;j<count;j++){
            formats[j].sType=VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;formats[j].pNext=0;
            formats[j].surfaceFormat=tmp[j];
        }
    *n=count;mv_free(tmp);
    return r;
}
static VkResult mv_vkCreateSwapchainKHR(VkDevice device,const VkSwapchainCreateInfoKHR *info,
    const VkAllocationCallbacks *a,VkSwapchainKHR *out) {
    (void)a;*out=VK_NULL_HANDLE;
    if(!info || !info->surface)return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_wsi_surface_object *s=(void *)info->surface;
    if(!s->model.alive)return VK_ERROR_SURFACE_LOST_KHR;
    const struct mv_wsi_helper *h=mv_wsi_bootstrap_helper();
    if(!h)return VK_ERROR_EXTENSION_NOT_PRESENT;
    const struct mv_wsi_ops *helper_ops=h->surface_ops?h->surface_ops(h->context,s->model.token):0;
    if(!helper_ops)return VK_ERROR_INITIALIZATION_FAILED;
    /* A new extent is a resize: invalidate older chains before creating this one. */
    if(s->model.extent.width!=info->imageExtent.width || s->model.extent.height!=info->imageExtent.height)
        mv_wsi_surface_resize(&s->model,info->imageExtent);
    struct mv_wsi_swapchain_object *sc=mv_alloc(sizeof(*sc));
    if(!sc)return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(sc,0,sizeof(*sc));
    sc->surface=s;sc->device=device;sc->helper_ops=helper_ops;
    sc->ops.context=sc;
    sc->ops.allocate=mv_wsi_bind_allocate;sc->ops.release=mv_wsi_bind_release;
    sc->ops.import=mv_wsi_bind_import;sc->ops.destroy=mv_wsi_bind_destroy;
    sc->ops.present=mv_wsi_bind_present;sc->ops.pace=mv_wsi_bind_pace;
    struct mv_wsi_swapchain *chain=0;
    VkResult r=mv_wsi_create(&s->model,info,&sc->ops,&chain);
    if(r){mv_free(sc);return r;}
    sc->model=chain;
    if(info->oldSwapchain){
        struct mv_wsi_swapchain_object *old=(void *)info->oldSwapchain;
        if(old->model)mv_wsi_retire(old->model);
    }
    *out=(VkSwapchainKHR)sc;
    return VK_SUCCESS;
}
static void mv_vkDestroySwapchainKHR(VkDevice device,VkSwapchainKHR swapchain,const VkAllocationCallbacks *a) {
    (void)device;(void)a;
    if(!swapchain)return;
    struct mv_wsi_swapchain_object *sc=(void *)swapchain;
    if(mv_wsi_destroy(sc->model)!=VK_SUCCESS)return;
    mv_free(sc);
}
static VkResult mv_vkGetSwapchainImagesKHR(VkDevice device,VkSwapchainKHR swapchain,uint32_t *n,VkImage *images) {
    (void)device;
    if(!swapchain || !n)return VK_ERROR_INITIALIZATION_FAILED;
    return mv_wsi_images(((struct mv_wsi_swapchain_object *)swapchain)->model,n,images);
}
/* Vulkan acquire/present must consume and signal the application's semaphores
 * and fences through a real queue operation. That synchronization layer is not
 * implemented; failing closed is deliberate and never reports a false present. */
static VkResult mv_wsi_loader_acquire(VkSwapchainKHR swapchain,uint64_t timeout,uint32_t *index) {
    struct mv_wsi_swapchain_object *sc=(void *)swapchain;
    return mv_wsi_acquire_paced(sc->model,index,timeout);
}
static VkResult mv_vkAcquireNextImageKHR(VkDevice device,VkSwapchainKHR swapchain,uint64_t timeout,
    VkSemaphore semaphore,VkFence fence,uint32_t *index) {
    (void)device;
    if(!swapchain || !index)return VK_ERROR_INITIALIZATION_FAILED;
    if(semaphore || fence)return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=mv_wsi_loader_acquire(swapchain,timeout,index);
    return r==VK_NOT_READY?VK_TIMEOUT:r;
}
static VkResult mv_vkAcquireNextImage2KHR(VkDevice device,const VkAcquireNextImageInfoKHR *info,uint32_t *index) {
    if(!info)return VK_ERROR_INITIALIZATION_FAILED;
    return mv_vkAcquireNextImageKHR(device,info->swapchain,info->timeout,info->semaphore,info->fence,index);
}
static VkResult mv_vkQueuePresentKHR(VkQueue queue,const VkPresentInfoKHR *info) {
    (void)queue;
    if(!info || info->swapchainCount!=1 || !info->pSwapchains || !info->pImageIndices)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(info->waitSemaphoreCount)return VK_ERROR_INITIALIZATION_FAILED;
    VkSwapchainKHR swapchain=info->pSwapchains[0];
    if(!swapchain)return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=mv_wsi_present(((struct mv_wsi_swapchain_object *)swapchain)->model,info->pImageIndices[0]);
    if(info->pResults)info->pResults[0]=r;
    return r;
}
#endif
static PFN_vkVoidFunction mv_wsi_lookup(const char *name) {
#if MV_WSI_PROTOTYPE
    if(!name || !mv_wsi_bootstrap_helper())return 0;
#define MV_WSI_ENTRY(n) if(!strcmp(name,#n))return (PFN_vkVoidFunction)mv_##n
    MV_WSI_ENTRY(vkCreateWaylandSurfaceKHR);MV_WSI_ENTRY(vkDestroySurfaceKHR);
    MV_WSI_ENTRY(vkGetPhysicalDeviceSurfaceSupportKHR);
    MV_WSI_ENTRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    MV_WSI_ENTRY(vkGetPhysicalDeviceSurfaceFormatsKHR);
    MV_WSI_ENTRY(vkGetPhysicalDeviceSurfacePresentModesKHR);
    MV_WSI_ENTRY(vkGetPhysicalDeviceWaylandPresentationSupportKHR);
    MV_WSI_ENTRY(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    MV_WSI_ENTRY(vkGetPhysicalDeviceSurfaceFormats2KHR);
    MV_WSI_ENTRY(vkCreateSwapchainKHR);MV_WSI_ENTRY(vkDestroySwapchainKHR);
    MV_WSI_ENTRY(vkGetSwapchainImagesKHR);MV_WSI_ENTRY(vkAcquireNextImageKHR);
    MV_WSI_ENTRY(vkAcquireNextImage2KHR);MV_WSI_ENTRY(vkQueuePresentKHR);
#undef MV_WSI_ENTRY
#else
    (void)name;
#endif
    return 0;
}
