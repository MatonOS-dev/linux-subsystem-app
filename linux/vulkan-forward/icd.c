#include <vulkan/vulkan_core.h>
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan_wayland.h>
#include <vulkan/vk_icd.h>
#include "runtime.h"
#include "backend.h"
#include "../gpu-forward-spike/bridge.h"
#include "dispatch.h"
#include "wsi.h"
#include "wayland_resolve.h"
#include "wsi_wayland.h"
#define EXPORT __attribute__((visibility("default")))
struct mv_instance;
struct mv_device;
struct mv_handle {
    VK_LOADER_DATA loader;
    void *android;
    struct mv_instance *instance;
    struct mv_handle *next;
    struct mv_device *device;
    VkCommandPool pool;
};
struct mv_instance {
    struct mv_handle base;
    VkAllocationCallbacks allocator;
    int custom, lock;
    int wsi_surface_enabled, wsi_wayland_enabled;
    struct mv_handle *physical;
    struct mv_dispatch dispatch;
    PFN_vkDestroyInstance destroy;
    PFN_vkEnumeratePhysicalDevices enumerate;
    PFN_vkEnumeratePhysicalDeviceGroups groups;
    PFN_vkCreateDevice create_device;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkEnumerateDeviceExtensionProperties extensions;
};
struct mv_device {
    struct mv_handle base;
    VkAllocationCallbacks allocator;
    int custom, lock, wsi_swapchain_enabled, wsi_sync_fd_enabled;
    struct mv_handle *children;
    struct mv_dispatch dispatch;
    PFN_vkGetDeviceProcAddr gdpa;
};
static void mv_vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice,uint32_t *,VkQueueFamilyProperties *);
static struct adopt_api *mv_adopt;
static struct gpu_bridge *mv_bridge;
static PFN_vkGetInstanceProcAddr mv_gipa;
static void *(*mv_ahb_native_handle)(const struct AHardwareBuffer *);
static void (*mv_ahb_describe)(const struct AHardwareBuffer *,void *);
static void (*mv_ahb_release)(struct AHardwareBuffer *);
static int (*mv_ahb_send_socket)(const struct AHardwareBuffer *,int);
static int mv_fast, mv_state;
static int mv_enter(struct mv_scope *s) {
    if(switch_to_android(mv_adopt,mv_fast,s))return 1;
    mv_diagnostic();return 0;
}
static int mv_init(void) {
    int state=__atomic_load_n(&mv_state,__ATOMIC_ACQUIRE);
    if(state==2)return 1;
    if(state==3)return 0;
    int expected=0;
    if(!__atomic_compare_exchange_n(&mv_state,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        while((state=__atomic_load_n(&mv_state,__ATOMIC_ACQUIRE))==1){}
        return state==2;
    }
    mv_bridge=(void *)mv_hex(mv_env("MATON_GPU_BRIDGE"));
    mv_adopt=(void *)mv_hex(mv_env("MATON_THREAD_ADOPT"));
    mv_fast=mv_hex(mv_env("MATON_FAST_TLS"))!=0;
    struct mv_scope s;
    if(mv_bridge && mv_bridge->version==1 && mv_enter(&s)) {
        void *lib=mv_bridge->open("/system/lib64/libvulkan.so",2);
        if(lib)mv_gipa=(PFN_vkGetInstanceProcAddr)mv_bridge->symbol(lib,"vkGetInstanceProcAddr");
        void *android=mv_bridge->open("/system/lib64/libandroid.so",2);
        if(android) {
            mv_ahb_native_handle=(void *)mv_bridge->symbol(android,"AHardwareBuffer_getNativeHandle");
            mv_ahb_describe=(void *)mv_bridge->symbol(android,"AHardwareBuffer_describe");
            mv_ahb_release=(void *)mv_bridge->symbol(android,"AHardwareBuffer_release");
            mv_ahb_send_socket=(void *)mv_bridge->symbol(android,"AHardwareBuffer_sendHandleToUnixSocket");
        }
        switch_back(&s);
    }
    __atomic_store_n(&mv_state,mv_gipa?2:3,__ATOMIC_RELEASE);
    if(!mv_gipa)mv_diagnostic();
    return mv_gipa!=0;
}
static void *mv_new(struct mv_instance *i,size_t n) {
    void *p=i->custom?i->allocator.pfnAllocation(i->allocator.pUserData,n,_Alignof(max_align_t),VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE):mv_alloc(n);
    if(p)memset(p,0,n);return p;
}
static void mv_delete(struct mv_instance *i,void *p) {
    if(i->custom)i->allocator.pfnFree(i->allocator.pUserData,p);else mv_free(p);
}
#include "wsi_vulkan.inc"
static VkPhysicalDevice mv_wrap(struct mv_instance *i,VkPhysicalDevice raw) {
    while(__atomic_exchange_n(&i->lock,1,__ATOMIC_ACQUIRE)){}
    struct mv_handle *h=i->physical;
    while(h && h->android!=(void *)raw)h=h->next;
    if(!h){h=mv_new(i,sizeof(*h));if(h){set_loader_magic_value(h);h->android=(void *)raw;h->instance=i;h->next=i->physical;i->physical=h;}}
    __atomic_store_n(&i->lock,0,__ATOMIC_RELEASE);
    return (VkPhysicalDevice)h;
}
#include "validate.inc"
#include "thunks.inc"
static VkResult mv_vkCreateInstance(const VkInstanceCreateInfo *info,const VkAllocationCallbacks *a,VkInstance *out) {
    *out=VK_NULL_HANDLE;
    if(!mv_init())return VK_ERROR_INCOMPATIBLE_DRIVER;
    for(uint32_t j=0;j<info->enabledExtensionCount;j++)
        if(!mv_wsi_instance_extension(info->ppEnabledExtensionNames[j]))return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(info->enabledLayerCount)return VK_ERROR_LAYER_NOT_PRESENT;
    if(a && (!a->pfnAllocation || !a->pfnFree || !a->pfnReallocation))return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_instance seed={0};if(a){seed.custom=1;seed.allocator=*a;}
    for(uint32_t j=0;j<info->enabledExtensionCount;j++) {
        if(!strcmp(info->ppEnabledExtensionNames[j],VK_KHR_SURFACE_EXTENSION_NAME))seed.wsi_surface_enabled=1;
        if(!strcmp(info->ppEnabledExtensionNames[j],VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME))seed.wsi_wayland_enabled=1;
    }
    if(seed.wsi_wayland_enabled && !seed.wsi_surface_enabled)return VK_ERROR_EXTENSION_NOT_PRESENT;
    struct mv_instance *i=mv_new(&seed,sizeof(*i));if(!i)return VK_ERROR_OUT_OF_HOST_MEMORY;
    i->custom=seed.custom;i->allocator=seed.allocator;
    i->wsi_surface_enabled=seed.wsi_surface_enabled;i->wsi_wayland_enabled=seed.wsi_wayland_enabled;
    VkInstanceCreateInfo copy=*info;
    /* No extension pNext data or Linux callback may reach bionic. */
    copy.pNext=0;
    copy.enabledExtensionCount=0;copy.ppEnabledExtensionNames=0;
    struct mv_scope s;
    if(!mv_enter(&s)){mv_delete(i,i);return VK_ERROR_INITIALIZATION_FAILED;}
    PFN_vkCreateInstance create=(PFN_vkCreateInstance)mv_gipa(0,"vkCreateInstance");
    VkInstance raw=0;
    VkResult result=create?create(&copy,0,&raw):VK_ERROR_INCOMPATIBLE_DRIVER;
    if(result==VK_SUCCESS){
        i->base.android=(void *)raw;i->base.instance=i;
        i->destroy=(PFN_vkDestroyInstance)mv_gipa(raw,"vkDestroyInstance");
        i->enumerate=(PFN_vkEnumeratePhysicalDevices)mv_gipa(raw,"vkEnumeratePhysicalDevices");
        i->groups=(PFN_vkEnumeratePhysicalDeviceGroups)mv_gipa(raw,"vkEnumeratePhysicalDeviceGroups");
        i->create_device=(PFN_vkCreateDevice)mv_gipa(raw,"vkCreateDevice");
        i->gdpa=(PFN_vkGetDeviceProcAddr)mv_gipa(raw,"vkGetDeviceProcAddr");
        i->extensions=(PFN_vkEnumerateDeviceExtensionProperties)mv_gipa(raw,"vkEnumerateDeviceExtensionProperties");
        mv_load_dispatch(i);
    }
    switch_back(&s);
    if(result!=VK_SUCCESS){mv_delete(i,i);return result;}
    set_loader_magic_value(i);*out=(VkInstance)i;return VK_SUCCESS;
}
static void mv_vkDestroyInstance(VkInstance instance,const VkAllocationCallbacks *a) {
    (void)a;if(!instance)return;struct mv_instance *i=(void *)instance;
    struct mv_scope s;if(!mv_enter(&s))return;
    i->destroy(i->base.android,0);switch_back(&s);
    struct mv_handle *h=i->physical;while(h){struct mv_handle *next=h->next;mv_delete(i,h);h=next;}
    mv_delete(i,i);
}
static VkResult mv_vkEnumeratePhysicalDevices(VkInstance instance,uint32_t *n,VkPhysicalDevice *devices) {
    struct mv_instance *i=(void *)instance;struct mv_scope s;
    if(!mv_enter(&s))return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=i->enumerate(i->base.android,n,devices);switch_back(&s);
    if(devices && (r==VK_SUCCESS || r==VK_INCOMPLETE))for(uint32_t j=0;j<*n;j++) {
        devices[j]=mv_wrap(i,devices[j]);if(!devices[j])return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return r;
}
static VkResult mv_vkEnumeratePhysicalDeviceGroups(VkInstance instance,uint32_t *n,VkPhysicalDeviceGroupProperties *groups) {
    struct mv_instance *i=(void *)instance;struct mv_scope s;
    if(!mv_enter(&s))return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=i->groups?i->groups(i->base.android,n,groups):VK_ERROR_FEATURE_NOT_PRESENT;
    switch_back(&s);
    if(groups && (r==VK_SUCCESS || r==VK_INCOMPLETE))for(uint32_t j=0;j<*n;j++)for(uint32_t k=0;k<groups[j].physicalDeviceCount;k++) {
        groups[j].physicalDevices[k]=mv_wrap(i,groups[j].physicalDevices[k]);
        if(!groups[j].physicalDevices[k])return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return r;
}
static VkResult mv_vkEnumerateInstanceVersion(uint32_t *v) {
    if(!mv_init())return VK_ERROR_INCOMPATIBLE_DRIVER;
    struct mv_scope s;if(!mv_enter(&s))return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateInstanceVersion f=(PFN_vkEnumerateInstanceVersion)mv_gipa(0,"vkEnumerateInstanceVersion");
    VkResult r=VK_SUCCESS;if(f)r=f(v);else *v=VK_API_VERSION_1_0;
    switch_back(&s);if(*v>VK_API_VERSION_1_4)*v=VK_API_VERSION_1_4;return r;
}
#include "extensions.inc"
static int mv_extension_allowed(const char *name) {
    for(size_t j=0;j<sizeof(mv_extensions)/sizeof(*mv_extensions);j++)if(!strcmp(name,mv_extensions[j]))return 1;
    return 0;
}
/* Instance WSI/debug callbacks are deliberately hidden. */
static VkResult mv_vkEnumerateInstanceExtensionProperties(const char *layer,uint32_t *n,VkExtensionProperties *p) {
    if(layer){*n=0;return VK_ERROR_LAYER_NOT_PRESENT;}
    return mv_wsi_instance_extensions(n,p);
}
static VkResult mv_vkEnumerateInstanceLayerProperties(uint32_t *n,VkLayerProperties *p) {(void)p;*n=0;return VK_SUCCESS;}
static VkResult mv_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical,const char *layer,uint32_t *n,VkExtensionProperties *out) {
    if(layer){*n=0;return VK_ERROR_LAYER_NOT_PRESENT;}
    struct mv_handle *h=(void *)physical;struct mv_scope s;
    if(!mv_enter(&s))return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateDeviceExtensionProperties f=h->instance->extensions;
    uint32_t count=0;VkResult r=f(h->android,0,&count,0);switch_back(&s);if(r!=VK_SUCCESS)return r;
    VkExtensionProperties *all=mv_alloc((size_t)count*sizeof(*all));if(!all)return VK_ERROR_OUT_OF_HOST_MEMORY;
    if(!mv_enter(&s)){mv_free(all);return VK_ERROR_INITIALIZATION_FAILED;}
    r=f(h->android,0,&count,all);switch_back(&s);
    if(r!=VK_SUCCESS){mv_free(all);return r;}
    uint32_t written=0,capacity=out?*n:0;
    for(uint32_t j=0;j<count;j++)if(mv_extension_allowed(all[j].extensionName)){if(out && written<capacity)out[written]=all[j];written++;}
    if(h->instance->wsi_wayland_enabled) {
        if(out && written<capacity) {
            memset(&out[written],0,sizeof(out[written]));
            memcpy(out[written].extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME,sizeof(VK_KHR_SWAPCHAIN_EXTENSION_NAME));
            out[written].specVersion=VK_KHR_SWAPCHAIN_SPEC_VERSION;
        }
        written++;
    }
    *n=out && written>capacity?capacity:written;mv_free(all);
    return out && written>capacity?VK_INCOMPLETE:VK_SUCCESS;
}
static VkResult mv_vkEnumerateDeviceLayerProperties(VkPhysicalDevice h,uint32_t *n,VkLayerProperties *p) {(void)h;return mv_vkEnumerateInstanceLayerProperties(n,p);}
static PFN_vkVoidFunction mv_vkGetInstanceProcAddr(VkInstance instance,const char *name);
#include "device.inc"
static PFN_vkVoidFunction mv_lookup(const char *name) {
    if(!name)return 0;
    PFN_vkVoidFunction wsi=mv_wsi_lookup(name);if(wsi)return wsi;
#include "lookup.inc"
    return 0;
}
static PFN_vkVoidFunction mv_vkGetInstanceProcAddr(VkInstance instance,const char *name) {
    if(!mv_init())return 0;
    PFN_vkVoidFunction local=mv_lookup(name);if(!local)return 0;
    if(mv_wsi_lookup(name))return local;
    struct mv_scope s;if(!mv_enter(&s))return 0;
    PFN_vkVoidFunction remote=mv_gipa(instance?((struct mv_instance *)instance)->base.android:0,name);
    switch_back(&s);return remote?local:0;
}
EXPORT VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *v) {if(*v<5)return VK_ERROR_INCOMPATIBLE_DRIVER;*v=5;return VK_SUCCESS;}
EXPORT PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance i,const char *n) {return mv_vkGetInstanceProcAddr(i,n);}
EXPORT PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(VkInstance i,const char *n) {if(!i || !n)return 0;
    const char *prefix="vkGetPhysicalDevice";
    for(unsigned j=0;prefix[j];j++)if(n[j]!=prefix[j])return 0;
    return mv_vkGetInstanceProcAddr(i,n);}
