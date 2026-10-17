struct mv_wayland_surface {
    struct mv_instance *instance;
    struct wl_display *display;
    struct wl_surface *surface;
    int custom_allocator;
    VkAllocationCallbacks allocator;
};

static const char *const mv_wsi_instance_names[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
};

static int mv_wsi_instance_extension(const char *name) {
    if (!name) return 0;
    for (size_t i=0;i<sizeof(mv_wsi_instance_names)/sizeof(*mv_wsi_instance_names);i++)
        if (!strcmp(name,mv_wsi_instance_names[i])) return 1;
    return 0;
}

static VkResult mv_wsi_instance_extensions(uint32_t *count,VkExtensionProperties *out) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t capacity=out?*count:0;
    uint32_t written=sizeof(mv_wsi_instance_names)/sizeof(*mv_wsi_instance_names);
    if (out) {
        for (uint32_t i=0;i<written && i<capacity;i++) {
            memset(&out[i],0,sizeof(out[i]));
            size_t n=0;while(mv_wsi_instance_names[i][n])n++;
            memcpy(out[i].extensionName,mv_wsi_instance_names[i],n+1);
            out[i].specVersion=i?VK_KHR_WAYLAND_SURFACE_SPEC_VERSION:VK_KHR_SURFACE_SPEC_VERSION;
        }
        *count=capacity<written?capacity:written;
        return capacity<written?VK_INCOMPLETE:VK_SUCCESS;
    }
    *count=written;return VK_SUCCESS;
}

static struct mv_wayland_surface *mv_wsi_surface(VkInstance instance,VkSurfaceKHR surface) {
    struct mv_wayland_surface *s=(void *)surface;
    return s && s->instance==(struct mv_instance *)instance?s:0;
}

static VkResult mv_vkCreateWaylandSurfaceKHR(VkInstance instance,const VkWaylandSurfaceCreateInfoKHR *info,
        const VkAllocationCallbacks *allocator,VkSurfaceKHR *out) {
    *out=VK_NULL_HANDLE;
    if (!instance || !info || info->sType!=VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR ||
        info->pNext || info->flags || !info->display || !info->surface ||
        (allocator && (!allocator->pfnAllocation || !allocator->pfnReallocation || !allocator->pfnFree)))
        return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_instance *i=(void *)instance;
    if (!i->wsi_surface_enabled || !i->wsi_wayland_enabled) return VK_ERROR_EXTENSION_NOT_PRESENT;
    int (*get_version)(void *)=(int (*)(void *))mv_wayland_symbol("wl_proxy_get_version");
    if (!get_version || get_version(info->surface)<1) return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_wayland_surface *s=allocator?
        allocator->pfnAllocation(allocator->pUserData,sizeof(*s),_Alignof(struct mv_wayland_surface),VK_SYSTEM_ALLOCATION_SCOPE_OBJECT):
        mv_new(i,sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(s,0,sizeof(*s));
    if (allocator) {
        s->custom_allocator=1;s->allocator=*allocator;
    }
    s->instance=i;s->display=info->display;s->surface=info->surface;
    *out=(VkSurfaceKHR)s;return VK_SUCCESS;
}

static void mv_vkDestroySurfaceKHR(VkInstance instance,VkSurfaceKHR surface,const VkAllocationCallbacks *allocator) {
    (void)allocator;
    struct mv_wayland_surface *s=mv_wsi_surface(instance,surface);
    if (s) {
        if (s->custom_allocator) s->allocator.pfnFree(s->allocator.pUserData,s);
        else mv_delete(s->instance,s);
    }
}

static VkBool32 mv_vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physical,
        uint32_t family,struct wl_display *display) {
    (void)display;
    struct mv_handle *h=(void *)physical;
    uint32_t count=0;
    mv_vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,0);
    if (!count || count>1024) return VK_FALSE;
    VkQueueFamilyProperties *props=mv_alloc((size_t)count*sizeof(*props));
    if (!props) return VK_FALSE;
    mv_vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,props);
    VkBool32 supported=family<count && (props[family].queueFlags&VK_QUEUE_GRAPHICS_BIT);
    mv_free(props);
    (void)h;
    return supported;
}

static VkResult mv_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physical,uint32_t family,
        VkSurfaceKHR surface,VkBool32 *supported) {
    struct mv_handle *h=(void *)physical;
    if (!supported || !mv_wsi_surface((VkInstance)h->instance,surface)) return VK_ERROR_SURFACE_LOST_KHR;
    *supported=mv_vkGetPhysicalDeviceWaylandPresentationSupportKHR(physical,family,
            ((struct mv_wayland_surface *)(void *)surface)->display);
    return VK_SUCCESS;
}

static VkResult mv_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physical,VkSurfaceKHR surface,
        VkSurfaceCapabilitiesKHR *caps) {
    struct mv_handle *h=(void *)physical;
    if (!caps || !mv_wsi_surface((VkInstance)h->instance,surface)) return VK_ERROR_SURFACE_LOST_KHR;
    *caps=(VkSurfaceCapabilitiesKHR){
        .minImageCount=2,.maxImageCount=4,
        .currentExtent={UINT32_MAX,UINT32_MAX},.minImageExtent={1,1},.maxImageExtent={4096,4096},
        .maxImageArrayLayers=1,.supportedTransforms=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .currentTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .supportedCompositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .supportedUsageFlags=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    return VK_SUCCESS;
}

static VkResult mv_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physical,VkSurfaceKHR surface,
        uint32_t *count,VkSurfaceFormatKHR *formats) {
    struct mv_handle *h=(void *)physical;
    if (!count || !mv_wsi_surface((VkInstance)h->instance,surface)) return VK_ERROR_SURFACE_LOST_KHR;
    if (!formats) {*count=1;return VK_SUCCESS;}
    if (!*count) return VK_INCOMPLETE;
    formats[0]=(VkSurfaceFormatKHR){VK_FORMAT_R8G8B8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    *count=1;return VK_SUCCESS;
}

static VkResult mv_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physical,VkSurfaceKHR surface,
        uint32_t *count,VkPresentModeKHR *modes) {
    struct mv_handle *h=(void *)physical;
    if (!count || !mv_wsi_surface((VkInstance)h->instance,surface)) return VK_ERROR_SURFACE_LOST_KHR;
    if (!modes) {*count=1;return VK_SUCCESS;}
    if (!*count) return VK_INCOMPLETE;
    modes[0]=VK_PRESENT_MODE_FIFO_KHR;*count=1;return VK_SUCCESS;
}

static PFN_vkVoidFunction mv_wsi_lookup(const char *name) {
    if (!name) return 0;
#define MV_WSI_PROC(n) if (!strcmp(name,#n)) return (PFN_vkVoidFunction)mv_##n
    MV_WSI_PROC(vkCreateWaylandSurfaceKHR);
    MV_WSI_PROC(vkDestroySurfaceKHR);
    MV_WSI_PROC(vkGetPhysicalDeviceWaylandPresentationSupportKHR);
    MV_WSI_PROC(vkGetPhysicalDeviceSurfaceSupportKHR);
    MV_WSI_PROC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    MV_WSI_PROC(vkGetPhysicalDeviceSurfaceFormatsKHR);
    MV_WSI_PROC(vkGetPhysicalDeviceSurfacePresentModesKHR);
#undef MV_WSI_PROC
    return 0;
}
