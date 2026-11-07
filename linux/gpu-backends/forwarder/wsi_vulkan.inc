struct mv_wayland_surface {
    struct mv_x11 *x11;
    struct mv_instance *instance;
    struct wl_display *display;
    struct wl_surface *surface;
    struct mv_wl_dmabuf *dmabuf;
    struct mv_wl_sync_surface *sync;
    int custom_allocator;
    VkAllocationCallbacks allocator;
    struct mv_wsi_surface state;
};

struct mv_wsi_live_context {
    struct mv_device *device;
    struct mv_wayland_surface *surface;
    PFN_vkCreateImage create_image;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkBindImageMemory bind_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkFreeMemory free_memory;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_ahb_properties;
    PFN_vkCreateSemaphore create_semaphore;
    PFN_vkDestroySemaphore destroy_semaphore;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
    PFN_vkCreateFence create_fence;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkWaitForFences wait_fences;
    PFN_vkResetFences reset_fences;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    PFN_vkImportFenceFdKHR import_fence_fd;
    /* Per image: the export semaphore of its last present and the fence of
     * that submission. Both stay alive until the fence signals (spec: no
     * destroy or reuse while a submission still references them). */
    /* Sharing of the swapchain being created (valid during creation only). */
    VkSharingMode sharing;uint32_t family_count;const uint32_t *families;
    int fifo;
    uint32_t slot_count;
    struct { VkSemaphore semaphore; VkFence fence; } *slots;
};
struct mv_ahb_desc { uint32_t width,height,layers,format; uint64_t usage; uint32_t stride,rfu0; uint64_t rfu1; };

static VkResult mv_wsi_live_allocate(void *opaque,uint64_t token,VkExtent2D extent,VkFormat format,const VkImageFormatListCreateInfo *views,
        struct AHardwareBuffer **ahb,VkImage *image,VkDeviceMemory *memory,uint64_t *image_token) {
    (void)token;struct mv_wsi_live_context *c=opaque;*ahb=0;*image=0;*memory=0;
    VkImageFormatListCreateInfo list={0};
    if(views)list=(VkImageFormatListCreateInfo){.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount=views->viewFormatCount,.pViewFormats=views->pViewFormats};
    VkExternalMemoryImageCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,.pNext=views?&list:0,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&external,.flags=views?VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT|VK_IMAGE_CREATE_EXTENDED_USAGE_BIT:0,.imageType=VK_IMAGE_TYPE_2D,
        .format=format,.extent={extent.width,extent.height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=MV_WSI_USAGE,
        .sharingMode=c->sharing,.queueFamilyIndexCount=c->sharing==VK_SHARING_MODE_CONCURRENT?c->family_count:0,
        .pQueueFamilyIndices=c->sharing==VK_SHARING_MODE_CONCURRENT?c->families:0,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    struct mv_scope scope;if(!mv_enter(&scope))return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_ahb_desc desc={.width=extent.width,.height=extent.height,.layers=1,.format=format==VK_FORMAT_B8G8R8A8_UNORM?5u:1u,
        .usage=(UINT64_C(1)<<8)|(UINT64_C(1)<<9)|(UINT64_C(1)<<11)}; /* GPU sampled/color output + compositor overlay */
    mv_trace_result("maton-vulkan: WSI entering client AHB allocator",mv_ahb_allocate?0:-1,0);
    int ar=mv_ahb_allocate?mv_ahb_allocate(&desc,ahb):-1;
    switch_back(&scope);
    mv_trace_result("maton-vulkan: WSI client AHB allocator returned",ar,*ahb?1:0);
    if(ar || !*ahb) { mv_trace_result("maton-vulkan: WSI client AHB allocation failed",ar,0);return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    if(!mv_enter(&scope)){if(mv_enter(&scope)){mv_ahb_release(*ahb);switch_back(&scope);}*ahb=0;return VK_ERROR_INITIALIZATION_FAILED;}
    VkAndroidHardwareBufferFormatPropertiesANDROID fp={.sType=VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID hp={.sType=VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,.pNext=&fp};
    mv_trace_result("maton-vulkan: WSI entering AHB Vulkan properties",0,0);
    VkResult r=c->get_ahb_properties(c->device->base.android,*ahb,&hp);
    mv_trace_result("maton-vulkan: WSI AHB Vulkan properties returned",r,hp.memoryTypeBits);
    if(r)mv_trace_result("maton-vulkan: WSI AHB properties failed",r,0);
    if(!r && (fp.format!=format || !(fp.formatFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) || !hp.memoryTypeBits || !hp.allocationSize))
        r=VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(r==VK_ERROR_FORMAT_NOT_SUPPORTED)mv_trace_result("maton-vulkan: WSI AHB properties unsupported",r,hp.memoryTypeBits);
    if(!r){r=c->create_image(c->device->base.android,&ci,0,image);if(r)mv_trace_result("maton-vulkan: WSI imported image create failed",r,0);}
    VkMemoryDedicatedAllocateInfo dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=*image};
    VkImportAndroidHardwareBufferInfoANDROID imported={.sType=VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .pNext=&dedicated,.buffer=*ahb};
    uint32_t type=0;while(!r && type<32 && !(hp.memoryTypeBits&(1u<<type)))type++;
    if(!r && type==32)r=VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&imported,
        .allocationSize=hp.allocationSize,.memoryTypeIndex=type};
    if(!r){r=c->allocate_memory(c->device->base.android,&ai,0,memory);if(r)mv_trace_result("maton-vulkan: WSI imported memory allocation failed",r,hp.memoryTypeBits);}
    if(!r){r=c->bind_image(c->device->base.android,*image,*memory,0);if(r)mv_trace_result("maton-vulkan: WSI imported image bind failed",r,0);}
    if(r)mv_trace_result("maton-vulkan: WSI AHB Vulkan import failed",r,hp.memoryTypeBits);
    switch_back(&scope);
    if(r) {
        if(*image && mv_enter(&scope)){c->destroy_image(c->device->base.android,*image,0);if(*memory)c->free_memory(c->device->base.android,*memory,0);switch_back(&scope);}
        *image=0;*memory=0;if(*ahb&&mv_ahb_release&&mv_enter(&scope)){mv_ahb_release(*ahb);switch_back(&scope);}*ahb=0;return r;
    }
    *image_token=(uint64_t)(uintptr_t)*ahb;return VK_SUCCESS;
}
static void mv_wsi_live_release(void *opaque,struct AHardwareBuffer *ahb,uint64_t token) {
    (void)opaque;(void)token;
    struct mv_scope scope;if(ahb&&mv_ahb_release&&mv_enter(&scope)){mv_ahb_release(ahb);switch_back(&scope);}
}
static void mv_wsi_live_destroy(void *opaque,VkImage image,VkDeviceMemory memory) {
    struct mv_wsi_live_context *c=opaque;struct mv_scope scope;if(!mv_enter(&scope))return;
    if(image)c->destroy_image(c->device->base.android,image,0);
    if(memory)c->free_memory(c->device->base.android,memory,0);
    switch_back(&scope);
}
static VkResult mv_wsi_live_create_buffer(void *opaque,struct AHardwareBuffer *ahb,VkExtent2D extent,void **buffer) {
    struct mv_wsi_live_context *c=opaque;
    if(c->surface->x11 || mv_wl_has_ahb_buffer(c->surface->dmabuf)) {
        int pair[2];int sp=mv_socketpair(pair);if(sp!=0){mv_trace_result("maton-vulkan: WSI socketpair failed",sp,0);return VK_ERROR_INITIALIZATION_FAILED;}
        struct mv_scope scope;
        if(!mv_enter(&scope)){mv_close(pair[0]);mv_close(pair[1]);return VK_ERROR_INITIALIZATION_FAILED;}
        int rc=mv_ahb_send_socket(ahb,pair[1]);
        switch_back(&scope);
        if(rc) {mv_trace_result("maton-vulkan: WSI AHB send-handle failed",rc,0);mv_close(pair[0]);mv_close(pair[1]);return VK_ERROR_INITIALIZATION_FAILED;}
        *buffer=c->surface->x11 ? (void *)mv_x11_buffer_create(c->surface->x11,pair[0],extent.width,extent.height,5u) :
            (void *)mv_wl_ahb_create_buffer(c->surface->dmabuf,extent.width,extent.height,0x34324241u,pair[0]);
        mv_close(pair[0]);mv_close(pair[1]);
        if(!*buffer){mv_trace_result(c->surface->x11?"maton-vulkan: WSI X11 buffer failed (stage*1000+errno)":"maton-vulkan: WSI Wayland buffer failed",c->surface->x11?mv_x11_last_error:0,0);return VK_ERROR_SURFACE_LOST_KHR;}
        return VK_SUCCESS;
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}
static void mv_wsi_live_destroy_buffer(void *opaque,void *buffer) {
    struct mv_wsi_live_context *c=opaque;
    if(c->surface->x11)mv_x11_buffer_destroy(buffer);else mv_wl_buffer_destroy(buffer);
}
static VkResult mv_wsi_live_present(void *opaque,uint64_t surface,uint64_t buffer,uint64_t serial,void *wl_buffer,int fence_fd) {
    (void)surface;(void)buffer;(void)serial;struct mv_wsi_live_context *c=opaque;
    if(c->surface->x11)return mv_x11_present(wl_buffer,fence_fd)?VK_SUCCESS:VK_ERROR_SURFACE_LOST_KHR;
    /* FIFO: each frame is shown for at least one compositor frame; wait for
     * the previous commit's frame callback instead of replacing it. */
    if(c->fifo&&mv_wl_sync_wait_frame(c->surface->sync)){mv_loud("maton-vulkan: Wayland FIFO frame wait failed",0);return VK_ERROR_SURFACE_LOST_KHR;}
    if(mv_wl_surface_present(c->surface->sync,c->surface->surface,wl_buffer,fence_fd)==0)return VK_SUCCESS;
    mv_loud("maton-vulkan: Wayland present commit failed (flush/listener)",fence_fd);
    return VK_ERROR_DEVICE_LOST;
}

static const char *const mv_wsi_instance_names[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    VK_KHR_XCB_SURFACE_EXTENSION_NAME,
    VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
};

static int mv_wsi_instance_extension(const char *name) {
    if (!name) return 0;
    for (size_t i=0;i<sizeof(mv_wsi_instance_names)/sizeof(*mv_wsi_instance_names);i++)
        if (!strcmp(name,mv_wsi_instance_names[i])) return 1;
    return 0;
}

static VkResult mv_wsi_instance_extensions(uint32_t *count,VkExtensionProperties *out) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    /* Apps may dlopen Wayland only after Vulkan extension discovery. These
     * extensions are implemented by this ICD; resolve client symbols when a
     * surface is created, once the app has loaded its Wayland library. */
    uint32_t capacity=out?*count:0;
    uint32_t written=sizeof(mv_wsi_instance_names)/sizeof(*mv_wsi_instance_names);
    if (out) {
        for (uint32_t i=0;i<written && i<capacity;i++) {
            memset(&out[i],0,sizeof(out[i]));
            size_t n=0;while(mv_wsi_instance_names[i][n])n++;
            memcpy(out[i].extensionName,mv_wsi_instance_names[i],n+1);
            out[i].specVersion=i==0?VK_KHR_SURFACE_SPEC_VERSION:i==1?VK_KHR_WAYLAND_SURFACE_SPEC_VERSION:i==2?VK_KHR_XCB_SURFACE_SPEC_VERSION:VK_KHR_XLIB_SURFACE_SPEC_VERSION;
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
    mv_trace_result("maton-vulkan: enter create Wayland surface",0,0);
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
    mv_wsi_surface_init(&s->state,(uint64_t)(uintptr_t)s,(VkExtent2D){0,0});
    s->dmabuf=mv_wl_dmabuf_create(info->display);
    if (!s->dmabuf) {
        if (s->custom_allocator) s->allocator.pfnFree(s->allocator.pUserData,s);
        else mv_delete(i,s);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *out=(VkSurfaceKHR)s;return VK_SUCCESS;
}

/* X11 surfaces share allocation/swapchain ownership with Wayland surfaces,
 * but their images use the private AHB import socket and stock Present release. */
static VkResult mv_create_x11_surface(VkInstance instance,void *connection,uint32_t window,
        const VkAllocationCallbacks *allocator,VkSurfaceKHR *out) {
    if(!out)return VK_ERROR_INITIALIZATION_FAILED;*out=VK_NULL_HANDLE;
    if(!instance||!connection||!window||(allocator&&(!allocator->pfnAllocation||!allocator->pfnReallocation||!allocator->pfnFree)))
        return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_instance *i=(void *)instance;
    if(!i->wsi_surface_enabled)return VK_ERROR_EXTENSION_NOT_PRESENT;
    struct mv_wayland_surface *s=allocator?
        allocator->pfnAllocation(allocator->pUserData,sizeof(*s),_Alignof(struct mv_wayland_surface),VK_SYSTEM_ALLOCATION_SCOPE_OBJECT):mv_new(i,sizeof(*s));
    if(!s)return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(s,0,sizeof(*s));s->instance=i;
    if(allocator){s->custom_allocator=1;s->allocator=*allocator;}
    s->x11=mv_x11_create(connection,window);
    if(!s->x11){if(allocator)allocator->pfnFree(allocator->pUserData,s);else mv_delete(i,s);return VK_ERROR_INITIALIZATION_FAILED;}
    mv_wsi_surface_init(&s->state,(uint64_t)(uintptr_t)s,(VkExtent2D){0,0});
    *out=(VkSurfaceKHR)s;return VK_SUCCESS;
}
static VkResult mv_vkCreateXcbSurfaceKHR(VkInstance instance,const VkXcbSurfaceCreateInfoKHR *info,
        const VkAllocationCallbacks *allocator,VkSurfaceKHR *out) {
    if(!instance||!((struct mv_instance *)instance)->wsi_xcb_enabled)return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(!info||info->sType!=VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR||info->pNext||info->flags)return VK_ERROR_INITIALIZATION_FAILED;
    return mv_create_x11_surface(instance,info->connection,info->window,allocator,out);
}
static VkResult mv_vkCreateXlibSurfaceKHR(VkInstance instance,const VkXlibSurfaceCreateInfoKHR *info,
        const VkAllocationCallbacks *allocator,VkSurfaceKHR *out) {
    if(!instance||!((struct mv_instance *)instance)->wsi_xlib_enabled)return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(!info||info->sType!=VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR||info->pNext||info->flags||info->window>UINT32_MAX)return VK_ERROR_INITIALIZATION_FAILED;
    return mv_create_x11_surface(instance,mv_x11_connection(info->dpy),info->window,allocator,out);
}
static void mv_vkDestroySurfaceKHR(VkInstance instance,VkSurfaceKHR surface,const VkAllocationCallbacks *allocator) {
    (void)allocator;
    struct mv_wayland_surface *s=mv_wsi_surface(instance,surface);
    if (s) {
        if(s->x11)mv_x11_destroy(s->x11);
        mv_wl_sync_surface_destroy(s->sync);
        mv_wl_dmabuf_destroy(s->dmabuf);
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

static VkBool32 mv_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physical,uint32_t family,
        xcb_connection_t *connection,xcb_visualid_t visual) {
    return mv_x11_visual_supported(connection,visual)?mv_vkGetPhysicalDeviceWaylandPresentationSupportKHR(physical,family,0):VK_FALSE;
}
static VkBool32 mv_vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physical,uint32_t family,
        Display *display,VisualID visual) {
    return mv_x11_visual_supported(mv_x11_connection(display),(uint32_t)visual)?mv_vkGetPhysicalDeviceWaylandPresentationSupportKHR(physical,family,0):VK_FALSE;
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
        .currentExtent={UINT32_MAX,UINT32_MAX},.minImageExtent={1,1},.maxImageExtent={MV_WSI_MAX_EXTENT,MV_WSI_MAX_EXTENT},
        .maxImageArrayLayers=1,.supportedTransforms=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .currentTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .supportedCompositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .supportedUsageFlags=MV_WSI_USAGE};
    struct mv_wayland_surface *s=mv_wsi_surface((VkInstance)h->instance,surface);
    if(s->x11) {
        uint32_t w,ht;if(!mv_x11_extent(s->x11,&w,&ht))return VK_ERROR_SURFACE_LOST_KHR;
        caps->currentExtent=caps->minImageExtent=caps->maxImageExtent=(VkExtent2D){w,ht};
        /* As Mesa: ARGB windows take premultiplied alpha, RGB ones opaque. */
        caps->supportedCompositeAlpha=VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR|
            (mv_x11_has_alpha(s->x11)?VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR:VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
    }
    return VK_SUCCESS;
}

static VkResult mv_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physical,VkSurfaceKHR surface,
        uint32_t *count,VkSurfaceFormatKHR *formats) {
    struct mv_handle *h=(void *)physical;
    if (!count || !mv_wsi_surface((VkInstance)h->instance,surface)) return VK_ERROR_SURFACE_LOST_KHR;
    if (!formats) {*count=1;return VK_SUCCESS;}
    if (!*count) return VK_INCOMPLETE;
    /* X11 visuals are BGRX/BGRA (what Zink/Kopper asks for); Wayland stays RGBA. */
    struct mv_wayland_surface *s=mv_wsi_surface((VkInstance)h->instance,surface);
    formats[0]=(VkSurfaceFormatKHR){s->x11?VK_FORMAT_B8G8R8A8_UNORM:VK_FORMAT_R8G8B8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    *count=1;return VK_SUCCESS;
}

static VkResult mv_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physical,VkSurfaceKHR surface,
        uint32_t *count,VkPresentModeKHR *modes) {
    struct mv_handle *h=(void *)physical;
    if (!count || !mv_wsi_surface((VkInstance)h->instance,surface)) return VK_ERROR_SURFACE_LOST_KHR;
    return mv_wsi_modes(count,modes);
}

static struct mv_wsi_live_context *mv_wsi_live(VkDevice device,VkSwapchainKHR swapchain) {
    if(!device||!swapchain)return 0;
    struct mv_wsi_swapchain *c=(void *)(uintptr_t)swapchain;
    struct mv_wsi_live_context *live=c->ops.context;
    return live&&live->device==(struct mv_device *)device?live:0;
}
static void mv_wsi_collect_releases(struct mv_wsi_swapchain *chain) {
    for(uint32_t j=0;j<chain->count;j++)if(((struct mv_wsi_live_context *)chain->ops.context)->surface->x11?mv_x11_released(chain->images[j].wl_buffer):mv_wl_buffer_released(chain->images[j].wl_buffer)) {
        if(!((struct mv_wsi_live_context *)chain->ops.context)->surface->x11)mv_wl_buffer_clear_released(chain->images[j].wl_buffer);
        (void)mv_wsi_release(chain,chain->images[j].token,chain->images[j].serial);
    }
}
static VkResult mv_create_swapchain(VkDevice device,const VkSwapchainCreateInfoKHR *info,
        const VkAllocationCallbacks *allocator,VkSwapchainKHR *out) {
    if(!out)return VK_ERROR_INITIALIZATION_FAILED;*out=VK_NULL_HANDLE;
    struct mv_device *d=(void *)device;
    if(!d||!d->wsi_swapchain_enabled||!info)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(allocator&&(!allocator->pfnAllocation||!allocator->pfnReallocation||!allocator->pfnFree))return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_wayland_surface *surface=mv_wsi_surface((VkInstance)d->base.instance,info->surface);
    if(!surface)return VK_ERROR_SURFACE_LOST_KHR;
    if(!mv_ahb_describe||!mv_ahb_release||!mv_ahb_allocate||!mv_ahb_send_socket)return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(surface->x11) {
        /* A window resized since the caps query is reported as SUBOPTIMAL on
         * acquire/present (as Mesa), not as a create failure. */
        if(info->imageFormat!=VK_FORMAT_B8G8R8A8_UNORM)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if(!surface->x11&&!surface->sync)surface->sync=mv_wl_sync_surface_create(surface->dmabuf,surface->surface);
    if(!surface->x11&&!surface->sync)return VK_ERROR_FEATURE_NOT_PRESENT;
    struct mv_wsi_live_context *c=mv_alloc(sizeof(*c));if(!c)return VK_ERROR_OUT_OF_HOST_MEMORY;memset(c,0,sizeof(*c));
    c->device=d;c->surface=surface;
    c->fifo=info->presentMode==VK_PRESENT_MODE_FIFO_KHR;
    c->sharing=info->imageSharingMode;c->family_count=info->queueFamilyIndexCount;c->families=info->pQueueFamilyIndices;
    struct mv_scope scope;
    if(!mv_enter(&scope)){mv_free(c);return VK_ERROR_INITIALIZATION_FAILED;}
    c->create_image=(PFN_vkCreateImage)d->gdpa(d->base.android,"vkCreateImage");
    c->allocate_memory=(PFN_vkAllocateMemory)d->gdpa(d->base.android,"vkAllocateMemory");
    c->bind_image=(PFN_vkBindImageMemory)d->gdpa(d->base.android,"vkBindImageMemory");
    c->destroy_image=(PFN_vkDestroyImage)d->gdpa(d->base.android,"vkDestroyImage");
    c->free_memory=(PFN_vkFreeMemory)d->gdpa(d->base.android,"vkFreeMemory");
    c->get_ahb_properties=(PFN_vkGetAndroidHardwareBufferPropertiesANDROID)d->gdpa(d->base.android,"vkGetAndroidHardwareBufferPropertiesANDROID");
    c->create_semaphore=(PFN_vkCreateSemaphore)d->gdpa(d->base.android,"vkCreateSemaphore");
    c->destroy_semaphore=(PFN_vkDestroySemaphore)d->gdpa(d->base.android,"vkDestroySemaphore");
    c->get_semaphore_fd=(PFN_vkGetSemaphoreFdKHR)d->gdpa(d->base.android,"vkGetSemaphoreFdKHR");
    c->create_fence=(PFN_vkCreateFence)d->gdpa(d->base.android,"vkCreateFence");
    c->destroy_fence=(PFN_vkDestroyFence)d->gdpa(d->base.android,"vkDestroyFence");
    c->wait_fences=(PFN_vkWaitForFences)d->gdpa(d->base.android,"vkWaitForFences");
    c->reset_fences=(PFN_vkResetFences)d->gdpa(d->base.android,"vkResetFences");
    c->import_semaphore_fd=(PFN_vkImportSemaphoreFdKHR)d->gdpa(d->base.android,"vkImportSemaphoreFdKHR");
    c->import_fence_fd=(PFN_vkImportFenceFdKHR)d->gdpa(d->base.android,"vkImportFenceFdKHR");
    switch_back(&scope);
    if(!c->create_image||!c->allocate_memory||!c->bind_image||!c->destroy_image||
       !c->free_memory||!c->get_ahb_properties||!c->create_semaphore||!c->destroy_semaphore||!c->get_semaphore_fd||
       !c->create_fence||!c->destroy_fence||!c->wait_fences||!c->reset_fences||!c->import_semaphore_fd||!c->import_fence_fd){mv_free(c);return VK_ERROR_EXTENSION_NOT_PRESENT;}
    VkExtent2D previous_extent=surface->state.extent;
    uint64_t previous_generation=surface->state.generation;
    mv_wsi_surface_resize(&surface->state,info->imageExtent);
    struct mv_wsi_ops ops={.context=c,.allocate=mv_wsi_live_allocate,.release=mv_wsi_live_release,
        .destroy=mv_wsi_live_destroy,.create_buffer=mv_wsi_live_create_buffer,
        .destroy_buffer=mv_wsi_live_destroy_buffer,.present=mv_wsi_live_present};
    struct mv_wsi_swapchain *chain=0;VkResult r=mv_wsi_create(&surface->state,info,&ops,&chain);
    /* The old swapchain is retired even when creation fails (spec), so the
     * generation bump stays; only the extent is rolled back. */
    if(r){surface->state.extent=previous_extent;if(!info->oldSwapchain)surface->state.generation=previous_generation;mv_free(c);return r;}
    c->slots=mv_alloc((size_t)chain->count*sizeof(*c->slots));
    if(!c->slots){mv_wsi_discard(chain);surface->state.extent=previous_extent;if(!info->oldSwapchain)surface->state.generation=previous_generation;mv_free(c);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    c->families=0;c->family_count=0;
    memset(c->slots,0,(size_t)chain->count*sizeof(*c->slots));c->slot_count=chain->count;
    if(surface->x11)mv_x11_set_mailbox(surface->x11,info->presentMode==VK_PRESENT_MODE_MAILBOX_KHR);
    if(surface->x11)mv_x11_set_extent(surface->x11,info->imageExtent.width,info->imageExtent.height);
    *out=(VkSwapchainKHR)(uintptr_t)chain;return VK_SUCCESS;
}
static VkResult mv_vkCreateSwapchainKHR(VkDevice device,const VkSwapchainCreateInfoKHR *info,
        const VkAllocationCallbacks *allocator,VkSwapchainKHR *out) {
    VkResult r=mv_create_swapchain(device,info,allocator,out);
    mv_trace_result("maton-vulkan: create swapchain",r,info?info->minImageCount:0);
    if(r<0)mv_loud("maton-vulkan: create swapchain failed, VkResult",(unsigned long)(long)r);
    return r;
}
static void mv_wsi_free_slots(struct mv_wsi_live_context *c) {
    struct mv_scope scope;if(!c->slots)return;
    if(!mv_enter(&scope)){mv_loud("maton-vulkan: swapchain destroy cannot enter Android scope; present sync objects leak",c->slot_count);return;}
    VkDevice dev=c->device->base.android;
    for(uint32_t j=0;j<c->slot_count;j++) {
        if(c->slots[j].fence) {
            VkResult w=c->wait_fences(dev,1,&c->slots[j].fence,VK_TRUE,UINT64_MAX);
            if(w)mv_loud("maton-vulkan: swapchain destroy present fence wait failed, VkResult",(unsigned long)(long)w);
            c->destroy_fence(dev,c->slots[j].fence,0);
        }
        if(c->slots[j].semaphore)c->destroy_semaphore(dev,c->slots[j].semaphore,0);
    }
    switch_back(&scope);mv_free(c->slots);c->slots=0;
}
static void mv_vkDestroySwapchainKHR(VkDevice device,VkSwapchainKHR swapchain,const VkAllocationCallbacks *allocator) {
    (void)allocator;struct mv_wsi_live_context *live=mv_wsi_live(device,swapchain);if(!live)return;
    mv_wsi_free_slots(live);
    struct mv_wsi_swapchain *c=(void *)(uintptr_t)swapchain;
    /* The compositor (X or Wayland) holds its own AHB references, so the
     * buffers can go while displayed. Waiting for the release of the final
     * displayed buffer would deadlock: it is released only on replacement. */
    mv_wsi_discard(c);
    mv_free(live);
}
static VkResult mv_vkGetSwapchainImagesKHR(VkDevice device,VkSwapchainKHR swapchain,uint32_t *count,VkImage *images) {
    struct mv_wsi_live_context *live=mv_wsi_live(device,swapchain);if(!live)return VK_ERROR_OUT_OF_DATE_KHR;
    return mv_wsi_images((void *)(uintptr_t)swapchain,count,images);
}
static uint64_t mv_wsi_now_ns(void) { return mv_monotonic_ns(); }
/* Freestanding i386 has no libgcc __udivdi3: plain shift-subtract division. */
static uint64_t mv_udiv64(uint64_t n,uint64_t d,uint64_t *rem) {
    uint64_t q=0,r=0;
    for(int i=63;i>=0;i--){r=r<<1|(n>>i&1);if(r>=d){r-=d;q|=1ull<<i;}}
    if(rem)*rem=r;return q;
}
static VkResult mv_vkAcquireNextImageKHR(VkDevice device,VkSwapchainKHR swapchain,uint64_t timeout,
        VkSemaphore semaphore,VkFence fence,uint32_t *index) {
    struct mv_wsi_live_context *live=mv_wsi_live(device,swapchain);if(!live)return VK_ERROR_OUT_OF_DATE_KHR;
    struct mv_wsi_swapchain *chain=(void *)(uintptr_t)swapchain;
    uint64_t start=timeout==UINT64_MAX?0:mv_wsi_now_ns();
    uint64_t deadline=timeout==UINT64_MAX||UINT64_MAX-start<timeout?UINT64_MAX:start+timeout;
    for(;;) {
        if(live->surface->x11&&mv_x11_dispatch(live->surface->x11,0)<0){mv_trace_result("maton-vulkan: acquire dispatch failed -> SURFACE_LOST",-1,0);return VK_ERROR_SURFACE_LOST_KHR;}
        if(!live->surface->x11)(void)mv_wl_dmabuf_dispatch(live->surface->dmabuf);
        mv_wsi_collect_releases(chain);
        VkResult r=mv_wsi_acquire(chain,index);
        if(r!=VK_NOT_READY) {
            if(r!=VK_SUCCESS){mv_trace_result("maton-vulkan: acquire failed",r,0);return r;}
            /* The image is idle once released, so signal the app's objects
             * with an already-signaled sync_fd (-1), as Mesa does; no
             * submission on an app queue the app may be using concurrently. */
            struct mv_device *d=(void *)device;struct mv_scope scope;
            if(!mv_enter(&scope)){mv_wsi_cancel_acquire(chain,*index);return VK_ERROR_INITIALIZATION_FAILED;}
            VkResult sr=VK_SUCCESS;
            if(semaphore){VkImportSemaphoreFdInfoKHR si={.sType=VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,.semaphore=semaphore,
                .flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,.fd=-1};
                sr=live->import_semaphore_fd(d->base.android,&si);}
            if(!sr&&fence){VkImportFenceFdInfoKHR fi={.sType=VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,.fence=fence,
                .flags=VK_FENCE_IMPORT_TEMPORARY_BIT,.handleType=VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,.fd=-1};
                sr=live->import_fence_fd(d->base.android,&fi);}
            switch_back(&scope);
            if(sr){mv_loud("maton-vulkan: acquire signal import failed, VkResult",(unsigned long)(long)sr);mv_wsi_cancel_acquire(chain,*index);return sr;}
            return live->surface->x11&&mv_x11_suboptimal(live->surface->x11)?VK_SUBOPTIMAL_KHR:VK_SUCCESS;
        }
        if(!timeout)return VK_NOT_READY;
        int wait_ms=-1;
        if(timeout!=UINT64_MAX) {
            uint64_t now=mv_wsi_now_ns();
            if(!now||now>=deadline)return VK_TIMEOUT;
            uint64_t remaining=deadline-now;
            uint64_t rem,rounded=mv_udiv64(remaining,1000000ull,&rem);rounded+=rem!=0;
            wait_ms=rounded>2147483647ull?2147483647:(int)rounded;
        }
        int dispatched=live->surface->x11?mv_x11_dispatch(live->surface->x11,wait_ms):mv_wl_dmabuf_dispatch_timeout(live->surface->dmabuf,wait_ms);
        if(dispatched==0)return VK_TIMEOUT;
        if(dispatched==-4)continue;
        if(dispatched<0){mv_trace_result("maton-vulkan: acquire wait failed -> SURFACE_LOST",dispatched,*index);return VK_ERROR_SURFACE_LOST_KHR;}
    }
}
static VkResult mv_vkAcquireNextImage2KHR(VkDevice device,const VkAcquireNextImageInfoKHR *info,uint32_t *index) {
    if(!info||info->pNext)return VK_ERROR_FEATURE_NOT_PRESENT;
    return mv_vkAcquireNextImageKHR(device,info->swapchain,info->timeout,info->semaphore,info->fence,index);
}
/* Ready a slot for a new present: its previous submission must be complete
 * before the semaphore and fence are reused (normally long done, since the
 * image was released and re-acquired since). */
static VkResult mv_wsi_slot_prepare(struct mv_wsi_live_context *c,uint32_t index) {
    if(index>=c->slot_count)return VK_ERROR_OUT_OF_DATE_KHR;
    VkDevice dev=c->device->base.android;VkResult r=VK_SUCCESS;
    if(c->slots[index].fence) {
        r=c->wait_fences(dev,1,&c->slots[index].fence,VK_TRUE,UINT64_MAX);
        if(!r)r=c->reset_fences(dev,1,&c->slots[index].fence);
        return r;
    }
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    r=c->create_fence(dev,&fi,0,&c->slots[index].fence);
    if(r){c->slots[index].fence=VK_NULL_HANDLE;return r;}
    VkExportSemaphoreCreateInfo export={.sType=VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkSemaphoreCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,.pNext=&export};
    r=c->create_semaphore(dev,&ci,0,&c->slots[index].semaphore);
    if(r){c->destroy_fence(dev,c->slots[index].fence,0);c->slots[index].fence=VK_NULL_HANDLE;c->slots[index].semaphore=VK_NULL_HANDLE;}
    return r;
}
static VkResult mv_vkQueuePresentKHR(VkQueue queue,const VkPresentInfoKHR *info) {
    if(!queue||!info||info->pNext||!info->swapchainCount||!info->pSwapchains||!info->pImageIndices)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct mv_handle *qh=(void *)queue;struct mv_device *d=qh->device;
    if(!d||!d->wsi_swapchain_enabled||!d->wsi_sync_fd_enabled)return VK_ERROR_EXTENSION_NOT_PRESENT;
    uint32_t n=info->swapchainCount;
    for(uint32_t k=0;k<n;k++)if(!mv_wsi_live((VkDevice)d,info->pSwapchains[k]))return VK_ERROR_OUT_OF_DATE_KHR;
    /* Everything that can fail without consuming the wait semaphores happens
     * before the submit; after it, failures are device loss (spec). */
    VkPipelineStageFlags *stages=0;VkSemaphore *signals=mv_alloc((size_t)n*sizeof(*signals));
    if(!signals)return VK_ERROR_OUT_OF_HOST_MEMORY;
    if(info->waitSemaphoreCount) {
        stages=mv_alloc((size_t)info->waitSemaphoreCount*sizeof(*stages));
        if(!stages){mv_free(signals);return VK_ERROR_OUT_OF_HOST_MEMORY;}
        for(uint32_t j=0;j<info->waitSemaphoreCount;j++)stages[j]=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    struct mv_scope scope;if(!mv_enter(&scope)){mv_free(signals);mv_free(stages);return VK_ERROR_INITIALIZATION_FAILED;}
    VkResult r=VK_SUCCESS;
    for(uint32_t k=0;k<n&&!r;k++) {
        struct mv_wsi_live_context *live=mv_wsi_live((VkDevice)d,info->pSwapchains[k]);
        r=mv_wsi_slot_prepare(live,info->pImageIndices[k]);
        if(!r)signals[k]=live->slots[info->pImageIndices[k]].semaphore;
    }
    if(r){switch_back(&scope);mv_free(signals);mv_free(stages);mv_loud("maton-vulkan: present sync prepare failed, VkResult",(unsigned long)(long)r);return r;}
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=info->waitSemaphoreCount,
        .pWaitSemaphores=info->pWaitSemaphores,.pWaitDstStageMask=stages,.signalSemaphoreCount=n,.pSignalSemaphores=signals};
    struct mv_wsi_live_context *first=mv_wsi_live((VkDevice)d,info->pSwapchains[0]);
    r=d->dispatch.vkQueueSubmit(qh->android,1,&submit,first->slots[info->pImageIndices[0]].fence);
    /* A batch-less submit signals its fence once all earlier work is done. */
    for(uint32_t k=1;k<n&&!r;k++) {
        struct mv_wsi_live_context *live=mv_wsi_live((VkDevice)d,info->pSwapchains[k]);
        r=d->dispatch.vkQueueSubmit(qh->android,0,0,live->slots[info->pImageIndices[k]].fence);
    }
    switch_back(&scope);mv_free(signals);mv_free(stages);
    if(r){mv_loud("maton-vulkan: present submit failed, VkResult",(unsigned long)(long)r);return r;}
    VkResult worst=VK_SUCCESS;
    for(uint32_t k=0;k<n;k++) {
        struct mv_wsi_live_context *live=mv_wsi_live((VkDevice)d,info->pSwapchains[k]);
        struct mv_wsi_swapchain *chain=(void *)(uintptr_t)info->pSwapchains[k];
        int fd=-1;VkResult pr=VK_ERROR_DEVICE_LOST;
        if(mv_enter(&scope)) {
            VkSemaphoreGetFdInfoKHR gi={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
                .semaphore=live->slots[info->pImageIndices[k]].semaphore,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
            pr=live->get_semaphore_fd(d->base.android,&gi,&fd);switch_back(&scope);
            if(pr){mv_loud("maton-vulkan: present semaphore export failed, VkResult",(unsigned long)(long)pr);pr=VK_ERROR_DEVICE_LOST;}
        }
        if(!pr){pr=mv_wsi_present(chain,info->pImageIndices[k],fd);mv_close(fd);}
        else if(fd>=0)mv_close(fd);
        if(pr==VK_ERROR_OUT_OF_HOST_MEMORY||pr==VK_ERROR_INITIALIZATION_FAILED){mv_loud("maton-vulkan: present failed after submit, VkResult",(unsigned long)(long)pr);pr=VK_ERROR_DEVICE_LOST;}
        if(pr==VK_SUCCESS&&live->surface->x11&&mv_x11_suboptimal(live->surface->x11))pr=VK_SUBOPTIMAL_KHR;
        if(info->pResults)info->pResults[k]=pr;
        /* Errors outrank SUBOPTIMAL, which outranks SUCCESS. */
        if(pr<0?(worst>=0):(worst==VK_SUCCESS))worst=pr;
    }
    return worst;
}

static VkResult mv_vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physical,VkSurfaceKHR surface,
        uint32_t *count,VkRect2D *rects) {
    VkSurfaceCapabilitiesKHR caps;VkResult r=mv_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps);
    if(r)return r;
    if(!rects){*count=1;return VK_SUCCESS;}
    if(!*count)return VK_INCOMPLETE;
    VkExtent2D e=caps.currentExtent.width==UINT32_MAX?caps.maxImageExtent:caps.currentExtent;
    rects[0]=(VkRect2D){{0,0},e};*count=1;return VK_SUCCESS;
}
static VkResult mv_vkGetDeviceGroupPresentCapabilitiesKHR(VkDevice device,VkDeviceGroupPresentCapabilitiesKHR *caps) {
    (void)device;if(!caps)return VK_ERROR_INITIALIZATION_FAILED;
    for(uint32_t j=0;j<VK_MAX_DEVICE_GROUP_SIZE;j++)caps->presentMask[j]=j==0;
    caps->modes=VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;return VK_SUCCESS;
}
static VkResult mv_vkGetDeviceGroupSurfacePresentModesKHR(VkDevice device,VkSurfaceKHR surface,VkDeviceGroupPresentModeFlagsKHR *modes) {
    struct mv_device *d=(void *)device;
    if(!d||!modes||!mv_wsi_surface((VkInstance)d->base.instance,surface))return VK_ERROR_SURFACE_LOST_KHR;
    *modes=VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;return VK_SUCCESS;
}

static PFN_vkVoidFunction mv_wsi_lookup(const char *name) {
    if (!name) return 0;
#define MV_WSI_PROC(n) if (!strcmp(name,#n)) return (PFN_vkVoidFunction)mv_##n
    MV_WSI_PROC(vkCreateWaylandSurfaceKHR);
    MV_WSI_PROC(vkCreateXcbSurfaceKHR);
    MV_WSI_PROC(vkCreateXlibSurfaceKHR);
    MV_WSI_PROC(vkGetPhysicalDeviceXcbPresentationSupportKHR);
    MV_WSI_PROC(vkGetPhysicalDeviceXlibPresentationSupportKHR);
    MV_WSI_PROC(vkDestroySurfaceKHR);
    MV_WSI_PROC(vkGetPhysicalDeviceWaylandPresentationSupportKHR);
    MV_WSI_PROC(vkGetPhysicalDeviceSurfaceSupportKHR);
    MV_WSI_PROC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    MV_WSI_PROC(vkGetPhysicalDeviceSurfaceFormatsKHR);
    MV_WSI_PROC(vkGetPhysicalDeviceSurfacePresentModesKHR);
    MV_WSI_PROC(vkGetPhysicalDevicePresentRectanglesKHR);
#undef MV_WSI_PROC
    return 0;
}

/* Device WSI is local; none of these commands are forwarded to Android. */
static PFN_vkVoidFunction mv_wsi_device_lookup(VkDevice device,const char *name) {
    if(!device||!name||!((struct mv_device *)device)->wsi_swapchain_enabled)return 0;
#define MV_WSI_DEVICE_PROC(n) if(!strcmp(name,#n))return (PFN_vkVoidFunction)mv_##n
    MV_WSI_DEVICE_PROC(vkCreateSwapchainKHR);
    MV_WSI_DEVICE_PROC(vkDestroySwapchainKHR);
    MV_WSI_DEVICE_PROC(vkGetSwapchainImagesKHR);
    MV_WSI_DEVICE_PROC(vkAcquireNextImageKHR);
    MV_WSI_DEVICE_PROC(vkAcquireNextImage2KHR);
    MV_WSI_DEVICE_PROC(vkQueuePresentKHR);
    MV_WSI_DEVICE_PROC(vkGetDeviceGroupPresentCapabilitiesKHR);
    MV_WSI_DEVICE_PROC(vkGetDeviceGroupSurfacePresentModesKHR);
#undef MV_WSI_DEVICE_PROC
    return 0;
}
