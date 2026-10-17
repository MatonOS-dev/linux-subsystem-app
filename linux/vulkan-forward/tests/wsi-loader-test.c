/* Host test for the loader-facing Wayland WSI layer (wsi-loader.inc).
 *
 * The Linux helper vtable and the resident import/query adapters are mocked on
 * app TLS; the real surface/swapchain model and command wiring run unchanged.
 * This is a host contract test: no Wayland socket, GPU, AHB or semaphore. */
#define _POSIX_C_SOURCE 200809L
#include "../wsi-loader.inc"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
/* --- helper transport ops (AHB allocation / present / pacing) ------------ */
static unsigned allocations,presents,pace_polls;
static VkResult transport_allocate(void *ctx,uint64_t surface,VkExtent2D e,VkFormat f,
    struct AHardwareBuffer **b,uint64_t *token) {
    (void)ctx;assert(surface==99 && e.width==640 && e.height==480 && f==VK_FORMAT_R8G8B8A8_UNORM);
    *token=++allocations;*b=(void *)(uintptr_t)(0x8000+*token);return VK_SUCCESS;
}
static void transport_release(void *ctx,struct AHardwareBuffer *b,uint64_t token) {
    (void)ctx;assert((uintptr_t)b==0x8000+token);
}
static VkResult transport_present(void *ctx,uint64_t surface,uint64_t buffer,uint64_t serial) {
    (void)ctx;(void)surface;(void)buffer;(void)serial;presents++;return VK_SUCCESS;
}
static VkResult transport_pace(void *ctx,uint64_t surface,uint64_t timeout) {
    (void)ctx;(void)timeout;assert(surface==99);pace_polls++;return VK_TIMEOUT;
}
static const struct mv_wsi_ops transport_ops={0,transport_allocate,transport_release,0,0,transport_present,transport_pace};
static const struct mv_wsi_ops *helper_ops_lookup(void *ctx,uint64_t token) {(void)ctx;assert(token==99);return &transport_ops;}
/* --- vtable published through the launcher bootstrap -------------------- */
static int helper_present=1;
static unsigned helper_creates,helper_destroys;
static VkResult helper_surface_create(void *ctx,void *display,void *surface,uint64_t *token,
    uint32_t *formats,uint32_t *count,uint64_t *usage) {
    (void)ctx;assert(display==(void *)(uintptr_t)0x1000 && surface==(void *)(uintptr_t)0x2000);
    formats[0]=1;formats[1]=5;*count=2;*usage=0xb00;*token=99;helper_creates++;return VK_SUCCESS;
}
static void helper_surface_destroy(void *ctx,uint64_t token) {(void)ctx;assert(token==99);helper_destroys++;}
static struct mv_wsi_helper helper={1,0,helper_surface_create,helper_surface_destroy,helper_ops_lookup,0,0};
static const struct mv_wsi_helper *mv_wsi_bootstrap_helper(void) {return helper_present?&helper:0;}
/* --- resident adapters (would enter bionic TLS in the ICD) --------------- */
static unsigned query_calls,imports,releases;
static VkResult mv_wsi_bridged_query(VkPhysicalDevice physical,const VkPhysicalDeviceImageFormatInfo2 *info,
    VkImageFormatProperties2 *out) {
    assert(physical==(VkPhysicalDevice)(uintptr_t)7);
    assert(info->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2);
    const VkPhysicalDeviceExternalImageFormatInfo *external=info->pNext;
    assert(external->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO &&
        external->handleType==VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
    assert(info->usage==(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT));
    assert(info->format==VK_FORMAT_R8G8B8A8_UNORM || info->format==VK_FORMAT_B8G8R8A8_UNORM);
    VkExternalImageFormatProperties *ep=out->pNext;
    VkAndroidHardwareBufferUsageANDROID *usage=ep->pNext;
    ep->externalMemoryProperties.externalMemoryFeatures=VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    ep->externalMemoryProperties.compatibleHandleTypes=external->handleType;
    usage->androidHardwareBufferUsage=0x300;
    out->imageFormatProperties=(VkImageFormatProperties){
        .maxExtent={info->format==VK_FORMAT_R8G8B8A8_UNORM?16384:8192,12288,1},
        .maxMipLevels=1,.maxArrayLayers=1,.sampleCounts=VK_SAMPLE_COUNT_1_BIT};
    query_calls++;return VK_SUCCESS;
}
static VkResult mv_wsi_resident_import(VkDevice device,struct AHardwareBuffer *ahb,VkExtent2D extent,
    VkFormat format,VkImage *image,VkDeviceMemory *memory) {
    assert(device==(VkDevice)(uintptr_t)0x3000 && ahb && extent.width==640 && extent.height==480 &&
        format==VK_FORMAT_R8G8B8A8_UNORM);
    *image=(VkImage)(uintptr_t)(0x4000+imports);*memory=(VkDeviceMemory)(uintptr_t)(0x5000+imports);
    imports++;return VK_SUCCESS;
}
static void mv_wsi_resident_release(VkDevice device,VkImage image,VkDeviceMemory memory) {
    assert(device==(VkDevice)(uintptr_t)0x3000 && image && memory);releases++;
}
int main(void) {
    /* Fail closed with no helper: no names, no extensions, no entry points. */
    helper_present=0;
    assert(!mv_wsi_lookup("vkCreateWaylandSurfaceKHR"));
    assert(!mv_wsi_instance_extension("VK_KHR_surface"));
    assert(!mv_wsi_instance_extension("VK_KHR_wayland_surface"));
    assert(!mv_wsi_device_extension("VK_KHR_swapchain"));
    uint32_t n=77;assert(!mv_wsi_enumerate_instance_extensions(&n,0) && n==0);
    VkSurfaceKHR absent=VK_NULL_HANDLE;
    VkWaylandSurfaceCreateInfoKHR absent_info={.sType=VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
    assert(mv_vkCreateWaylandSurfaceKHR(0,&absent_info,0,&absent)==VK_ERROR_EXTENSION_NOT_PRESENT && !absent);
    assert(!mv_wsi_device_extension_count());
    helper_present=1;
    /* With a helper: the surface/swapchain set is advertised and resolvable. */
    assert(mv_wsi_instance_extension("VK_KHR_surface") && mv_wsi_instance_extension("VK_KHR_wayland_surface"));
    assert(mv_wsi_instance_extension("VK_KHR_get_surface_capabilities2"));
    assert(!mv_wsi_instance_extension("VK_EXT_debug_utils"));
    assert(mv_wsi_device_extension("VK_KHR_swapchain"));
    assert(!mv_wsi_device_extension("VK_KHR_surface"));
    n=0;assert(!mv_wsi_enumerate_instance_extensions(&n,0) && n==3);
    VkExtensionProperties props[3];uint32_t small=2;assert(mv_wsi_enumerate_instance_extensions(&small,props)==VK_INCOMPLETE && small==2);
    n=3;assert(!mv_wsi_enumerate_instance_extensions(&n,props) && n==3);
    assert(!strcmp(props[0].extensionName,"VK_KHR_surface") && props[0].specVersion==1);
    assert(!strcmp(props[1].extensionName,"VK_KHR_wayland_surface"));
    assert(!strcmp(props[2].extensionName,"VK_KHR_get_surface_capabilities2"));
    assert(mv_wsi_lookup("vkCreateWaylandSurfaceKHR") && mv_wsi_lookup("vkCreateSwapchainKHR"));
    assert(mv_wsi_lookup("vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
    assert(!mv_wsi_lookup("vkCreateInstance"));
    assert(mv_wsi_device_extension_count()==1);
    VkExtensionProperties de;mv_wsi_device_extension_at(0,&de);
    assert(!strcmp(de.extensionName,"VK_KHR_swapchain") && de.specVersion==1);
    /* Surface creation and lazy resident attachment. */
    VkWaylandSurfaceCreateInfoKHR info={.sType=VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display=(void *)(uintptr_t)0x1000,.surface=(void *)(uintptr_t)0x2000};
    VkSurfaceKHR surface=VK_NULL_HANDLE;
    assert(!mv_vkCreateWaylandSurfaceKHR((VkInstance)(uintptr_t)0x1111,&info,0,&surface) && surface);
    assert(helper_creates==1);
    VkPhysicalDevice physical=(VkPhysicalDevice)(uintptr_t)7;
    VkBool32 supported=VK_FALSE;
    assert(!mv_vkGetPhysicalDeviceSurfaceSupportKHR(physical,0,surface,&supported) && supported);
    assert(query_calls==2);
    VkSurfaceCapabilitiesKHR caps;
    assert(!mv_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps));
    assert(caps.minImageCount==2 && !caps.maxImageCount && caps.currentExtent.width==UINT32_MAX &&
        caps.currentExtent.height==UINT32_MAX && caps.maxImageExtent.width==8192 &&
        caps.supportedUsageFlags==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    VkPhysicalDeviceSurfaceInfo2KHR surface2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,.surface=surface};
    VkSurfaceCapabilities2KHR caps2={.sType=VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
    assert(!mv_vkGetPhysicalDeviceSurfaceCapabilities2KHR(physical,&surface2,&caps2));
    assert(caps2.surfaceCapabilities.minImageCount==2 && caps2.surfaceCapabilities.currentExtent.width==UINT32_MAX);
    uint32_t formats=0;assert(!mv_vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&formats,0) && formats==2);
    VkSurfaceFormatKHR format;formats=1;assert(mv_vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&formats,&format)==VK_INCOMPLETE);
    formats=2;assert(!mv_vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&formats,&format) && format.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    VkSurfaceFormat2KHR format2[2]={{.sType=VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR},{.sType=VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR}};
    formats=2;assert(!mv_vkGetPhysicalDeviceSurfaceFormats2KHR(physical,&surface2,&formats,format2));
    assert(format2[0].surfaceFormat.format==VK_FORMAT_R8G8B8A8_UNORM && format2[0].sType==VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR);
    uint32_t modes=0;assert(!mv_vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&modes,0) && modes==1);
    VkPresentModeKHR mode;modes=1;assert(!mv_vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&modes,&mode) && mode==VK_PRESENT_MODE_FIFO_KHR);
    assert(mv_vkGetPhysicalDeviceWaylandPresentationSupportKHR(physical,0,(struct wl_display *)0)==VK_TRUE);
    /* Swapchain creation binds the helper transport and the resident importer. */
    VkDevice device=(VkDevice)(uintptr_t)0x3000;
    VkSwapchainCreateInfoKHR ci={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=surface,
        .minImageCount=2,.imageFormat=VK_FORMAT_R8G8B8A8_UNORM,.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent={640,480},.imageArrayLayers=1,.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,.presentMode=VK_PRESENT_MODE_FIFO_KHR};
    VkSwapchainKHR swapchain=VK_NULL_HANDLE;
    VkSwapchainCreateInfoKHR bad=ci;bad.presentMode=VK_PRESENT_MODE_MAILBOX_KHR;
    assert(mv_vkCreateSwapchainKHR(device,&bad,0,&swapchain)==VK_ERROR_FORMAT_NOT_SUPPORTED && !swapchain);
    assert(!mv_vkCreateSwapchainKHR(device,&ci,0,&swapchain) && swapchain);
    assert(allocations==2 && imports==2);
    /* Extent is now concrete after creation. */
    assert(!mv_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps) && caps.currentExtent.width==640);
    uint32_t images=0;assert(!mv_vkGetSwapchainImagesKHR(device,swapchain,&images,0) && images==2);
    VkImage list[2];images=2;assert(!mv_vkGetSwapchainImagesKHR(device,swapchain,&images,list));
    assert(list[0]!=list[1]);
    /* Acquire/present refuse the unimplemented semaphore/fence signalling path. */
    uint32_t index=0;
    assert(mv_vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,(VkSemaphore)(uintptr_t)1,0,&index)==VK_ERROR_INITIALIZATION_FAILED);
    assert(!mv_vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,0,0,&index));
    VkPresentInfoKHR present={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,
        .pWaitSemaphores=(VkSemaphore[]){ (VkSemaphore)(uintptr_t)1 },.swapchainCount=1,
        .pSwapchains=(VkSwapchainKHR[]){swapchain},.pImageIndices=(uint32_t[]){index}};
    assert(mv_vkQueuePresentKHR((VkQueue)(uintptr_t)0x6000,&present)==VK_ERROR_INITIALIZATION_FAILED);
    present.waitSemaphoreCount=0;
    assert(!mv_vkQueuePresentKHR((VkQueue)(uintptr_t)0x6000,&present) && presents==1);
    /* Pacing surfaces VK_TIMEOUT rather than a false present. */
    assert(!mv_vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,0,0,&index));
    assert(!mv_vkQueuePresentKHR((VkQueue)(uintptr_t)0x6000,&present));
    assert(mv_vkAcquireNextImageKHR(device,swapchain,0,0,0,&index)==VK_TIMEOUT && pace_polls==1);
    /* oldSwapchain retirement lets a queued predecessor be destroyed. */
    ci.oldSwapchain=swapchain;
    VkSwapchainKHR next=VK_NULL_HANDLE;
    assert(!mv_vkCreateSwapchainKHR(device,&ci,0,&next) && next && next!=swapchain);
    assert(allocations==4 && imports==4);
    mv_vkDestroySwapchainKHR(device,swapchain,0);
    mv_vkDestroySwapchainKHR(device,next,0);
    assert(releases==4);
    mv_vkDestroySurfaceKHR((VkInstance)(uintptr_t)0x1111,surface,0);
    assert(helper_destroys==1);
    puts("PASS WSI loader: helper-gated extensions/entry points, surface negotiation, lazy resident attach, "
         "variable currentExtent, surface queries/caps2/formats2, swapchain images, semaphore fail-closed, "
         "FIFO pacing timeout, oldSwapchain retirement");
    return 0;
}
