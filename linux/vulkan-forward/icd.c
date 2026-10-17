#include <vulkan/vulkan_core.h>
#include <vulkan/vk_icd.h>
#include "runtime.h"
#include "backend.h"
#include "../gpu-forward-spike/bridge.h"
#include "dispatch.h"
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
    int custom, lock;
    struct mv_handle *children;
    struct mv_dispatch dispatch;
    PFN_vkGetDeviceProcAddr gdpa;
};
static struct adopt_api *mv_adopt;
static struct gpu_bridge *mv_bridge;
static PFN_vkGetInstanceProcAddr mv_gipa;
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
#include "wsi-loader.inc"
/* Loader-facing Wayland WSI. The Linux helper vtable is published through the
 * launcher bootstrap; the resident adapters below bracket bionic TLS and use
 * only raw Android function pointers, never a Linux callback. */
static const struct mv_wsi_helper *mv_wsi_bootstrap_helper(void) {
#if MV_WSI_PROTOTYPE
    const char *value=mv_env("MATON_WSI_HELPER");
    if(!value)return 0;
    uintptr_t address=mv_hex(value);
    if(!address)return 0;
    const struct mv_wsi_helper *helper=(const void *)address;
    return helper->version==1?helper:0;
#else
    return 0;
#endif
}
#if MV_WSI_PROTOTYPE
static const char mv_ahb_extension[]="VK_ANDROID_external_memory_android_hardware_buffer";
static int mv_android_has_device_extension(struct mv_handle *h,const char *name) {
    struct mv_scope scope;if(!mv_enter(&scope))return 0;
    PFN_vkEnumerateDeviceExtensionProperties f=h->instance->extensions;
    uint32_t count=0;VkResult r=f?f(h->android,0,&count,0):VK_ERROR_INITIALIZATION_FAILED;
    switch_back(&scope);
    if(r!=VK_SUCCESS || !count)return 0;
    VkExtensionProperties *all=mv_alloc((size_t)count*sizeof(*all));
    if(!all)return 0;
    if(!mv_enter(&scope)){mv_free(all);return 0;}
    r=f(h->android,0,&count,all);switch_back(&scope);
    int found=0;
    if(r==VK_SUCCESS)for(uint32_t j=0;j<count;j++)if(!strcmp(all[j].extensionName,name)){found=1;break;}
    mv_free(all);return found;
}
static VkResult mv_wsi_bridged_query(VkPhysicalDevice physical,const VkPhysicalDeviceImageFormatInfo2 *info,
    VkImageFormatProperties2 *out) {
    struct mv_handle *h=(void *)physical;struct mv_scope scope;
    if(!mv_enter(&scope))return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 query=h->instance->dispatch.vkGetPhysicalDeviceImageFormatProperties2;
    VkResult r=query?query(h->android,info,out):VK_ERROR_INITIALIZATION_FAILED;
    switch_back(&scope);return r;
}
static VkResult mv_wsi_resident_import(VkDevice device,struct AHardwareBuffer *ahb,VkExtent2D extent,
    VkFormat format,VkImage *image,VkDeviceMemory *memory) {
    struct mv_device *d=(void *)device;struct mv_scope scope;
    if(!mv_enter(&scope))return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID properties=(PFN_vkGetAndroidHardwareBufferPropertiesANDROID)d->gdpa(d->base.android,"vkGetAndroidHardwareBufferPropertiesANDROID");
    PFN_vkCreateImage create=(PFN_vkCreateImage)d->gdpa(d->base.android,"vkCreateImage");
    PFN_vkAllocateMemory allocate=(PFN_vkAllocateMemory)d->gdpa(d->base.android,"vkAllocateMemory");
    PFN_vkBindImageMemory bind=(PFN_vkBindImageMemory)d->gdpa(d->base.android,"vkBindImageMemory");
    PFN_vkDestroyImage destroy=(PFN_vkDestroyImage)d->gdpa(d->base.android,"vkDestroyImage");
    PFN_vkFreeMemory free=(PFN_vkFreeMemory)d->gdpa(d->base.android,"vkFreeMemory");
    struct mv_wsi_driver driver={d->base.android,properties,create,allocate,bind,destroy,free};
    VkResult r=mv_wsi_import_ahb(&driver,ahb,extent,format,image,memory);
    switch_back(&scope);return r;
}
static void mv_wsi_resident_release(VkDevice device,VkImage image,VkDeviceMemory memory) {
    struct mv_device *d=(void *)device;struct mv_scope scope;
    if(!mv_enter(&scope))return;
    PFN_vkDestroyImage destroy=(PFN_vkDestroyImage)d->gdpa(d->base.android,"vkDestroyImage");
    PFN_vkFreeMemory free=(PFN_vkFreeMemory)d->gdpa(d->base.android,"vkFreeMemory");
    if(image && destroy)destroy(d->base.android,image,0);
    if(memory && free)free(d->base.android,memory,0);
    switch_back(&scope);
}
#endif
static VkResult mv_vkCreateInstance(const VkInstanceCreateInfo *info,const VkAllocationCallbacks *a,VkInstance *out) {
    *out=VK_NULL_HANDLE;
    if(!mv_init())return VK_ERROR_INCOMPATIBLE_DRIVER;
    for(uint32_t j=0;j<info->enabledExtensionCount;j++)
        if(!mv_wsi_instance_extension(info->ppEnabledExtensionNames[j]))return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(info->enabledLayerCount)return VK_ERROR_LAYER_NOT_PRESENT;
    if(a && (!a->pfnAllocation || !a->pfnFree || !a->pfnReallocation))return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_instance seed={0};if(a){seed.custom=1;seed.allocator=*a;}
    struct mv_instance *i=mv_new(&seed,sizeof(*i));if(!i)return VK_ERROR_OUT_OF_HOST_MEMORY;
    i->custom=seed.custom;i->allocator=seed.allocator;
    VkInstanceCreateInfo copy=*info;
    /* No extension pNext data or Linux callback may reach bionic. Our WSI
     * extensions are ours alone and are backed by the Linux helper. */
    copy.pNext=0;
    copy.enabledExtensionCount=0;
    copy.ppEnabledExtensionNames=0;
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
/* Instance WSI/debug callbacks are deliberately hidden; only the helper-backed
 * Wayland surface set is exposed when a helper is present. */
static VkResult mv_vkEnumerateInstanceExtensionProperties(const char *layer,uint32_t *n,VkExtensionProperties *p) {
    if(layer){*n=0;return VK_ERROR_LAYER_NOT_PRESENT;}
    return mv_wsi_enumerate_instance_extensions(n,p);
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
    /* Our own helper-backed device extensions are advertised independently of
     * the resident driver's list; the driver list may not contain them. */
    for(uint32_t j=0;j<mv_wsi_device_extension_count();j++){if(out && written<capacity)mv_wsi_device_extension_at(j,&out[written]);written++;}
    *n=out && written>capacity?capacity:written;mv_free(all);
    return out && written>capacity?VK_INCOMPLETE:VK_SUCCESS;
}
static VkResult mv_vkEnumerateDeviceLayerProperties(VkPhysicalDevice h,uint32_t *n,VkLayerProperties *p) {(void)h;return mv_vkEnumerateInstanceLayerProperties(n,p);}
static PFN_vkVoidFunction mv_vkGetInstanceProcAddr(VkInstance instance,const char *name);
#include "device.inc"
static PFN_vkVoidFunction mv_lookup(const char *name) {
    if(!name)return 0;
#include "lookup.inc"
    return 0;
}
static PFN_vkVoidFunction mv_vkGetInstanceProcAddr(VkInstance instance,const char *name) {
    if(!mv_init())return 0;
    /* Loader-facing WSI lives entirely in this DSO and has no Android remote. */
    PFN_vkVoidFunction wsi=mv_wsi_lookup(name);if(wsi)return wsi;
    PFN_vkVoidFunction local=mv_lookup(name);if(!local)return 0;
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
