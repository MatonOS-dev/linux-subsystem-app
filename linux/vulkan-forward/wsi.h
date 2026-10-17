#ifndef MV_WSI_H
#define MV_WSI_H
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_android.h>
/* D1 internal API, NOT a loader ABI. Caller serializes all object access.
 * No pointers in a wire message; tokens are authenticated connection-local IDs.
 * Callbacks execute on Linux TLS. A resident adapter must bracket bionic calls. */
struct mv_wsi_surface { uint64_t token, generation; VkExtent2D extent; int alive; };
struct mv_wsi_image { VkImage image; VkDeviceMemory memory; struct AHardwareBuffer *ahb;
    uint64_t token, serial; unsigned state; void *wl_buffer;
    int dmabuf_fd; uint32_t drm_format, stride, offset; uint64_t modifier; };
struct mv_wsi_ops {
    void *context;
    /* Allocate an exportable Android Vulkan image and return the referenced
     * AHB obtained from its dedicated export memory. */
    VkResult (*allocate)(void *,uint64_t,VkExtent2D,VkFormat,
        struct AHardwareBuffer **,VkImage *,VkDeviceMemory *,uint64_t *);
    void (*release)(void *,struct AHardwareBuffer *,uint64_t);
    void (*destroy)(void *,VkImage,VkDeviceMemory);
    /* Export the just-allocated AHB's verified dma-buf plane. The returned
     * fd is owned by the callback's platform boundary and must remain valid
     * until destroy_buffer; it is not closed by the state machine. */
    VkResult (*export_dmabuf)(void *,struct AHardwareBuffer *,VkImage,
        int *,uint32_t *,uint32_t *,uint32_t *,uint64_t *);
    VkResult (*create_buffer)(void *,VkExtent2D,uint32_t,int,uint32_t,uint32_t,uint64_t,void **);
    void (*destroy_buffer)(void *,void *);
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
VkResult mv_wsi_formats(uint32_t *,VkSurfaceFormatKHR *);
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
