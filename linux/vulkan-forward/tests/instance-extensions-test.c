/* Test enumeration before libwayland is loaded, as dlopen-based WSI clients do. */
#include "../icd.c"
#include <assert.h>
#include <stdio.h>
static struct adopt_thread thread;
static struct adopt_thread *current(void) {return &thread;}
static VkResult VKAPI_CALL capture_device(VkPhysicalDevice physical,const VkDeviceCreateInfo *info,
                                         const VkAllocationCallbacks *a,VkDevice *out) {
    (void)physical;(void)a;(void)out;
    assert(info->enabledExtensionCount==5);
    unsigned ahb=0,sync=0;
    for(uint32_t i=0;i<info->enabledExtensionCount;i++) {
        assert(strcmp(info->ppEnabledExtensionNames[i],VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        ahb+=!strcmp(info->ppEnabledExtensionNames[i],"VK_ANDROID_external_memory_android_hardware_buffer");
        sync+=!strcmp(info->ppEnabledExtensionNames[i],VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    }
    assert(ahb==1 && sync==1);
    return VK_ERROR_DEVICE_LOST; /* Avoid needing a real Android driver. */
}
int main(void) {
    uint32_t count=0;
    assert(mv_wsi_instance_extensions(&count,0)==VK_SUCCESS && count==2);
    VkExtensionProperties props[2];
    count=1;
    assert(mv_wsi_instance_extensions(&count,props)==VK_INCOMPLETE && count==1);
    assert(!strcmp(props[0].extensionName,VK_KHR_SURFACE_EXTENSION_NAME));
    count=2;
    assert(mv_wsi_instance_extensions(&count,props)==VK_SUCCESS && count==2);
    assert(!strcmp(props[1].extensionName,VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME));
    assert(mv_wsi_instance_extension(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME));
    assert(!mv_wsi_instance_extension("VK_KHR_xcb_surface"));
    thread.resident_fs=mv_tp_read(0);
    struct adopt_api api={.version=1,.current=current};mv_adopt=&api;
    struct mv_instance instance={.wsi_wayland_enabled=1,.create_device=capture_device};
    struct mv_handle physical={.instance=&instance};
    const char *extensions[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
    VkDeviceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.enabledExtensionCount=2,.ppEnabledExtensionNames=extensions};
    VkDevice device;
    assert(mv_vkCreateDevice((VkPhysicalDevice)&physical,&ci,0,&device)==VK_ERROR_DEVICE_LOST);
    puts("PASS WSI discovery before Wayland dlopen and private device dependencies without duplicates");
}
