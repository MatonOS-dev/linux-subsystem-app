#ifndef MV_WSI_H
#define MV_WSI_H
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_android.h>
/* D1 internal API, NOT a loader ABI. Caller serializes all object access.
 * No pointers in a wire message; tokens are authenticated connection-local IDs.
 * Callbacks execute on Linux TLS. A resident adapter must bracket bionic calls. */
/* Queried on resident TLS for one physical device and negotiated transport.
 * Not a published helper ABI. Transport formats are protocol enum values,
 * not VkFormat values. No compositor policy limit is a driver capability. */
struct mv_wsi_limits {
    VkExtent2D max_extent;
    VkSurfaceFormatKHR formats[2];
    uint32_t format_count;
};
VkResult mv_wsi_query_limits(VkPhysicalDevice,PFN_vkGetPhysicalDeviceImageFormatProperties2,
    uint32_t,const uint32_t *,uint64_t,struct mv_wsi_limits *);
struct mv_wsi_surface { uint64_t token, generation; VkExtent2D extent; int alive;
    struct mv_wsi_limits limits; };
struct mv_wsi_image { VkImage image; VkDeviceMemory memory; struct AHardwareBuffer *ahb;
    uint64_t token, serial; unsigned state; };
struct mv_wsi_ops {
    void *context;
    VkResult (*allocate)(void *,uint64_t,VkExtent2D,VkFormat,struct AHardwareBuffer **,uint64_t *);
    void (*release)(void *,struct AHardwareBuffer *,uint64_t);
    VkResult (*import)(void *,struct AHardwareBuffer *,VkExtent2D,VkFormat,VkImage *,VkDeviceMemory *);
    void (*destroy)(void *,VkImage,VkDeviceMemory);
    /* Enqueue only after render completion; D1 has no GPU sync integration.
     * Failure MUST mean not enqueued. Success retains the buffer until release. */
    VkResult (*present)(void *,uint64_t,uint64_t,uint64_t);
};
struct mv_wsi_swapchain { struct mv_wsi_surface *surface; struct mv_wsi_ops ops;
    uint64_t generation, serial; uint32_t count; struct mv_wsi_image *images; };
void mv_wsi_surface_init(struct mv_wsi_surface *,uint64_t,VkExtent2D);
void mv_wsi_surface_resize(struct mv_wsi_surface *,VkExtent2D);
VkBool32 mv_wsi_support(const struct mv_wsi_surface *,VkQueueFlags,int);
VkResult mv_wsi_capabilities(const struct mv_wsi_surface *,VkSurfaceCapabilitiesKHR *);
VkResult mv_wsi_formats(const struct mv_wsi_surface *,uint32_t *,VkSurfaceFormatKHR *);
VkResult mv_wsi_modes(uint32_t *,VkPresentModeKHR *);
VkResult mv_wsi_create(struct mv_wsi_surface *,const VkSwapchainCreateInfoKHR *,const struct mv_wsi_ops *,struct mv_wsi_swapchain **);
VkResult mv_wsi_images(struct mv_wsi_swapchain *,uint32_t *,VkImage *);
VkResult mv_wsi_acquire(struct mv_wsi_swapchain *,uint32_t *);
VkResult mv_wsi_present(struct mv_wsi_swapchain *,uint32_t);
VkResult mv_wsi_release(struct mv_wsi_swapchain *,uint64_t,uint64_t);
/* Nonblocking prototype teardown refuses in-flight buffers; D2 needs retirement. */
VkResult mv_wsi_destroy(struct mv_wsi_swapchain *);
/* Runs entirely on resident TLS with raw device/function pointers. */
struct mv_wsi_driver { VkDevice device;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID properties;
    PFN_vkCreateImage create; PFN_vkAllocateMemory allocate; PFN_vkBindImageMemory bind;
    PFN_vkDestroyImage destroy; PFN_vkFreeMemory free;
};
VkResult mv_wsi_import_ahb(struct mv_wsi_driver *,struct AHardwareBuffer *,VkExtent2D,VkFormat,VkImage *,VkDeviceMemory *);
#endif
