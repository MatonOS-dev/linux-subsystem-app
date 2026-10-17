#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include "../bridge.h"
#include "client.h"
#include "switch.h"
static struct gpu_bridge *bridge;
static const char *library;
static void *run(void *unused) {
    (void)unused;
    struct shadow_lease *lease = shadow_current();
    if (!lease) return (void *)1;
    uintptr_t app = gs_read();
    gs_write(lease->fs);
    void *handle = bridge->open(library, 2); /* RTLD_NOW in both libcs */
    PFN_vkGetInstanceProcAddr gipa = handle ? (PFN_vkGetInstanceProcAddr)bridge->symbol(handle,"vkGetInstanceProcAddr") : NULL;
    PFN_vkCreateInstance create = gipa ? (PFN_vkCreateInstance)gipa(NULL,"vkCreateInstance") : NULL;
    VkInstance instance = VK_NULL_HANDLE;
    VkInstanceCreateInfo info = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkResult result = create ? create(&info,NULL,&instance) : VK_ERROR_INITIALIZATION_FAILED;
    uint32_t count = 0;
    VkPhysicalDevice devices[16];
    VkPhysicalDeviceProperties properties = {0};
    if (result == VK_SUCCESS) {
        PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(instance,"vkEnumeratePhysicalDevices");
        PFN_vkGetPhysicalDeviceProperties get_properties = (PFN_vkGetPhysicalDeviceProperties)gipa(instance,"vkGetPhysicalDeviceProperties");
        PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(instance,"vkDestroyInstance");
        if (!enumerate || !get_properties || !destroy) __builtin_trap();
        result = enumerate(instance,&count,NULL);
        if (result == VK_SUCCESS && count) {
            if (count > 16) count = 16;
            result = enumerate(instance,&count,devices);
            if (result == VK_SUCCESS || result == VK_INCOMPLETE) get_properties(devices[0],&properties);
        }
        destroy(instance,NULL);
    }
    gs_write(app);
    printf("resident loader on leased TLS: result=%d physicalDevices=%u first=%s\n",result,count,properties.deviceName);
    return (void *)(uintptr_t)(result != VK_SUCCESS || !count);
}
int main(int argc, char **argv) {
    const char *value = getenv("MATON_GPU_BRIDGE");
    if (!value) return 2;
    bridge = (void *)(uintptr_t)strtoull(value,NULL,16);
    if (bridge->version != 1) return 3;
    library = argc > 1 ? argv[1] : "libvulkan.so.1";
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,run,NULL));
    void *result;
    assert(!pthread_join(thread,&result));
    return (int)(uintptr_t)result;
}
