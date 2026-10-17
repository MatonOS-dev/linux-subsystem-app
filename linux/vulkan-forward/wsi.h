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
    struct mv_wsi_limits limits;
    /* Wayland transport binding. All helper calls are app-TLS callbacks;
     * only the transport session (opaque token) is retained here. */
    const struct mv_wsi_helper *helper;
    uint32_t transport_formats[2], transport_format_count;
    uint64_t transport_usage;
    VkPhysicalDevice driver_device; };
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
    /* Optional FIFO pacing: wait for a frame callback (0 = poll). NULL means
     * the caller paces itself. Failure MUST NOT be treated as presented. */
    VkResult (*pace)(void *,uint64_t,uint64_t);
};
struct mv_wsi_swapchain { struct mv_wsi_surface *surface; struct mv_wsi_ops ops;
    uint64_t generation, serial; uint32_t count; struct mv_wsi_image *images;
    int retired; };
/* Linux-side Wayland helper (the ICD owns no libwayland/libc dependency).
 * Runs on the application's Linux TLS; the resident adapters bracket bionic. */
struct mv_wsi_helper {
    uint64_t version;
    void *context;
    VkResult (*surface_create)(void *,void *display,void *surface,
        uint64_t *token,uint32_t *formats,uint32_t *format_count,uint64_t *usage);
    void (*surface_destroy)(void *,uint64_t token);
    const struct mv_wsi_ops *(*surface_ops)(void *,uint64_t token);
    VkResult (*surface_frame)(void *,uint64_t token,uint64_t timeout_ns);
    VkResult (*dispatch)(void *);
};
void mv_wsi_surface_init(struct mv_wsi_surface *,uint64_t,VkExtent2D);
/* Bind a live transport session to the model surface. */
VkResult mv_wsi_surface_negotiate(struct mv_wsi_surface *,const struct mv_wsi_helper *,
    void *display,void *surface);
void mv_wsi_surface_retire(struct mv_wsi_surface *);
VkResult mv_wsi_surface_attach_device(struct mv_wsi_surface *,VkPhysicalDevice,
    PFN_vkGetPhysicalDeviceImageFormatProperties2);
void mv_wsi_surface_resize(struct mv_wsi_surface *,VkExtent2D);
VkBool32 mv_wsi_support(const struct mv_wsi_surface *,VkQueueFlags,int);
VkResult mv_wsi_capabilities(const struct mv_wsi_surface *,VkSurfaceCapabilitiesKHR *);
VkResult mv_wsi_formats(const struct mv_wsi_surface *,uint32_t *,VkSurfaceFormatKHR *);
VkResult mv_wsi_modes(uint32_t *,VkPresentModeKHR *);
VkResult mv_wsi_create(struct mv_wsi_surface *,const VkSwapchainCreateInfoKHR *,const struct mv_wsi_ops *,struct mv_wsi_swapchain **);
VkResult mv_wsi_images(struct mv_wsi_swapchain *,uint32_t *,VkImage *);
VkResult mv_wsi_acquire(struct mv_wsi_swapchain *,uint32_t *);
VkResult mv_wsi_acquire_paced(struct mv_wsi_swapchain *,uint32_t *,uint64_t timeout_ns);
VkResult mv_wsi_present(struct mv_wsi_swapchain *,uint32_t);
VkResult mv_wsi_release(struct mv_wsi_swapchain *,uint64_t,uint64_t);
/* Mark a swapchain replaced by a newer one. Its acquired images stay valid
 * until presented/released; mv_wsi_destroy may then drop queued images. */
void mv_wsi_retire(struct mv_wsi_swapchain *);
/* Retires any in-flight buffers (they complete on their own), ending the
 * caller's obligation. Safe on a swapchain whose images are still queued. */
VkResult mv_wsi_destroy(struct mv_wsi_swapchain *);
/* Runs entirely on resident TLS with raw device/function pointers. */
struct mv_wsi_driver { VkDevice device;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID properties;
    PFN_vkCreateImage create; PFN_vkAllocateMemory allocate; PFN_vkBindImageMemory bind;
    PFN_vkDestroyImage destroy; PFN_vkFreeMemory free;
};
VkResult mv_wsi_import_ahb(struct mv_wsi_driver *,struct AHardwareBuffer *,VkExtent2D,VkFormat,VkImage *,VkDeviceMemory *);
#endif
