#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>
static unsigned allocations,frees;
static void *VKAPI_CALL allocate(void *user,size_t n,size_t align,VkSystemAllocationScope scope) {
    assert(user==&allocations);assert(scope==VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    void *p=0;assert(!posix_memalign(&p,align,n));allocations++;return p;
}
static void *VKAPI_CALL reallocate(void *user,void *p,size_t n,size_t align,VkSystemAllocationScope scope) {
    (void)user;(void)p;(void)n;(void)align;(void)scope;abort();
}
static void VKAPI_CALL release(void *user,void *p) {assert(user==&allocations);frees++;free(p);}
int main(int argc,char **argv) {
    assert(argc==2);int absent_adoption=getenv("MATON_QUERY_NO_ADOPT")!=0;
    if(absent_adoption)unsetenv("MATON_THREAD_ADOPT");
    void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!lib){fprintf(stderr,"%s\n",dlerror());return 1;}
    PFN_vk_icdNegotiateLoaderICDInterfaceVersion negotiate=(void *)dlsym(lib,"vk_icdNegotiateLoaderICDInterfaceVersion");
    PFN_vkGetInstanceProcAddr gipa=(void *)dlsym(lib,"vk_icdGetInstanceProcAddr");assert(negotiate && gipa);
    uint32_t version=7;assert(negotiate(&version)==VK_SUCCESS && version==5);
    PFN_vkCreateInstance create=(void *)gipa(0,"vkCreateInstance");
    if(!getenv("MATON_GPU_BRIDGE") || absent_adoption){assert(!create);puts("PASS: absent bridge/adoption fails closed");return 0;}
    assert(create);
    PFN_vkEnumerateInstanceVersion getversion=(void *)gipa(0,"vkEnumerateInstanceVersion");
    assert(getversion && getversion(&version)==VK_SUCCESS);printf("Android instance API %u.%u.%u\n",VK_VERSION_MAJOR(version),VK_VERSION_MINOR(version),VK_VERSION_PATCH(version));
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkAllocationCallbacks allocator={.pUserData=&allocations,.pfnAllocation=allocate,.pfnReallocation=reallocate,.pfnFree=release};
    for(unsigned pass=0;pass<2;pass++) {
        VkInstance instance=0;const VkAllocationCallbacks *a=pass?&allocator:0;
        assert(create(&info,a,&instance)==VK_SUCCESS);assert(valid_loader_magic_value(instance));
#define LOAD(name) PFN_##name name=(void *)gipa(instance,#name);assert(name)
        LOAD(vkDestroyInstance);LOAD(vkEnumeratePhysicalDevices);LOAD(vkEnumeratePhysicalDeviceGroups);
        LOAD(vkGetPhysicalDeviceProperties);LOAD(vkGetPhysicalDeviceProperties2);LOAD(vkGetPhysicalDeviceFeatures2);
        LOAD(vkGetPhysicalDeviceMemoryProperties);LOAD(vkGetPhysicalDeviceQueueFamilyProperties);LOAD(vkGetPhysicalDeviceFormatProperties);
        LOAD(vkEnumerateDeviceExtensionProperties);
        assert(!gipa(instance,"vkCreateDevice"));assert(!gipa(instance,"vkCreateSwapchainKHR"));
        uint32_t n=0;assert(vkEnumeratePhysicalDevices(instance,&n,0)==VK_SUCCESS && n);
        VkPhysicalDevice *devices=calloc(n,sizeof(*devices)),*again=calloc(n,sizeof(*again));
        assert(vkEnumeratePhysicalDevices(instance,&n,devices)==VK_SUCCESS);
        uint32_t count=n;assert(vkEnumeratePhysicalDevices(instance,&count,again)==VK_SUCCESS);
        assert(devices[0]==again[0] && valid_loader_magic_value(devices[0]));
        VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(devices[0],&props);
        VkPhysicalDeviceDriverProperties driver={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&driver};
        vkGetPhysicalDeviceProperties2(devices[0],&props2);
        assert(!strcmp(props.deviceName,props2.properties.deviceName));
        printf("pass=%u devices=%u name=%s driver=%s API=%u.%u.%u\n",pass,n,props.deviceName,driver.driverName,VK_VERSION_MAJOR(props.apiVersion),VK_VERSION_MINOR(props.apiVersion),VK_VERSION_PATCH(props.apiVersion));
        VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};vkGetPhysicalDeviceFeatures2(devices[0],&features);
        VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(devices[0],&memory);assert(memory.memoryTypeCount);
        uint32_t queues=0;vkGetPhysicalDeviceQueueFamilyProperties(devices[0],&queues,0);assert(queues);
        VkFormatProperties format;vkGetPhysicalDeviceFormatProperties(devices[0],VK_FORMAT_R8G8B8A8_UNORM,&format);assert(format.optimalTilingFeatures);
        uint32_t ext=100;assert(vkEnumerateDeviceExtensionProperties(devices[0],0,&ext,0)==VK_SUCCESS && !ext);
        uint32_t groups=0;assert(vkEnumeratePhysicalDeviceGroups(instance,&groups,0)==VK_SUCCESS && groups);
        VkPhysicalDeviceGroupProperties *gp=calloc(groups,sizeof(*gp));
        for(uint32_t j=0;j<groups;j++)gp[j].sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
        assert(vkEnumeratePhysicalDeviceGroups(instance,&groups,gp)==VK_SUCCESS);
        assert(gp[0].physicalDevices[0]==devices[0]);
        free(gp);free(again);free(devices);vkDestroyInstance(instance,a);
    }
    assert(allocations && allocations==frees);
    printf("PASS: instance/physical queries, pNext properties, stable handle identity/groups, allocator callbacks %u/%u\n",allocations,frees);
}
