struct mv_wayland_surface {
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
    PFN_vkGetImageMemoryRequirements get_requirements;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkBindImageMemory bind_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkFreeMemory free_memory;
    PFN_vkGetMemoryAndroidHardwareBufferANDROID get_ahb;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier;
    PFN_vkGetImageSubresourceLayout get_layout;
};
struct mv_native_handle { int version, num_fds, num_ints; int data[]; };
struct mv_ahb_desc { uint32_t width,height,layers,format; uint64_t usage; uint32_t stride,rfu0; uint64_t rfu1; };

static VkResult mv_wsi_live_allocate(void *opaque,uint64_t token,VkExtent2D extent,VkFormat format,
        struct AHardwareBuffer **ahb,VkImage *image,VkDeviceMemory *memory,uint64_t *image_token) {
    (void)token;struct mv_wsi_live_context *c=opaque;*ahb=0;*image=0;*memory=0;
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier={
        .sType=VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier=0,.drmFormatModifierPlaneCount=1,
        .pPlaneLayouts=(VkSubresourceLayout[1]){{.offset=0,.rowPitch=(VkDeviceSize)extent.width*4}}};
    VkExternalMemoryImageCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext=&modifier,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&external,.imageType=VK_IMAGE_TYPE_2D,
        .format=format,.extent={extent.width,extent.height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    struct mv_scope scope;if(!mv_enter(&scope))return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=c->create_image(c->device->base.android,&ci,0,image);
    if(r)mv_trace_result("maton-vulkan: WSI image create failed",r,0);
    VkMemoryRequirements req={0};if(!r)c->get_requirements(c->device->base.android,*image,&req);
    VkMemoryDedicatedAllocateInfo dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=*image};
    VkExportMemoryAllocateInfo exported={.sType=VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,.pNext=&dedicated,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    uint32_t type=0;if(!r){while(type<32 && !(req.memoryTypeBits&(1u<<type)))type++;if(type==32)r=VK_ERROR_OUT_OF_DEVICE_MEMORY;}
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&exported,
        .allocationSize=req.size,.memoryTypeIndex=type};
    if(!r)r=c->allocate_memory(c->device->base.android,&ai,0,memory);
    if(r)mv_trace_result("maton-vulkan: WSI memory selection/allocation failed",r,req.memoryTypeBits);
    if(!r)r=c->bind_image(c->device->base.android,*image,*memory,0);
    if(!r) {
        VkMemoryGetAndroidHardwareBufferInfoANDROID gi={.sType=VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,.memory=*memory};
        r=c->get_ahb(c->device->base.android,&gi,ahb);
        if(r)mv_trace_result("maton-vulkan: WSI AHB export failed",r,0);
    }
    switch_back(&scope);
    if(r) {
        if(*image && mv_enter(&scope)){c->destroy_image(c->device->base.android,*image,0);if(*memory)c->free_memory(c->device->base.android,*memory,0);switch_back(&scope);}
        *image=0;*memory=0;if(*ahb&&mv_ahb_release&&mv_enter(&scope)){mv_ahb_release(*ahb);switch_back(&scope);}*ahb=0;return r;
    }
    *image_token=(uint64_t)(uintptr_t)*ahb;return VK_SUCCESS;
}
static void mv_wsi_live_release(void *opaque,struct AHardwareBuffer *ahb,uint64_t token) {
    (void)opaque;(void)token;struct mv_scope scope;if(ahb&&mv_ahb_release&&mv_enter(&scope)){mv_ahb_release(ahb);switch_back(&scope);}
}
static void mv_wsi_live_destroy(void *opaque,VkImage image,VkDeviceMemory memory) {
    struct mv_wsi_live_context *c=opaque;struct mv_scope scope;if(!mv_enter(&scope))return;
    if(image)c->destroy_image(c->device->base.android,image,0);
    if(memory)c->free_memory(c->device->base.android,memory,0);
    switch_back(&scope);
}
static VkResult mv_wsi_live_export(void *opaque,struct AHardwareBuffer *ahb,VkImage image,int *fd,
        uint32_t *format,uint32_t *stride,uint32_t *offset,uint64_t *modifier) {
    struct mv_wsi_live_context *c=opaque;
    *fd=-1;*format=0x34324241u;*stride=0;*offset=0;*modifier=0;
    struct mv_scope scope;
    /* The private AHB transport is authoritative when available. Do not
     * inspect or assume any native-handle fd in that case. */
    if(mv_wl_has_ahb_buffer(c->surface->dmabuf) && mv_ahb_send_socket) {
        if(!mv_ahb_describe||!mv_enter(&scope))return VK_ERROR_EXTENSION_NOT_PRESENT;
        struct mv_ahb_desc desc={0};mv_ahb_describe(ahb,&desc);
        switch_back(&scope);
        if(!desc.width||!desc.height||desc.stride>UINT32_MAX/4)return VK_ERROR_FORMAT_NOT_SUPPORTED;
        *stride=desc.stride*4;
        return VK_SUCCESS;
    }
    if(!mv_ahb_native_handle||!mv_ahb_describe||!mv_enter(&scope))return VK_ERROR_EXTENSION_NOT_PRESENT;
    struct mv_ahb_desc desc={0};mv_ahb_describe(ahb,&desc);
    const struct mv_native_handle *native=mv_ahb_native_handle(ahb);
    VkImageDrmFormatModifierPropertiesEXT mp={.sType=VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
    VkSubresourceLayout layout={0};VkImageSubresource sub={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.mipLevel=0,.arrayLayer=0};
    VkResult r=!native||native->num_fds<1||desc.format!=1||desc.width==0||desc.height==0?VK_ERROR_FORMAT_NOT_SUPPORTED:VK_SUCCESS;
    if(!r)r=c->get_modifier(c->device->base.android,image,&mp);
    if(!r)c->get_layout(c->device->base.android,image,&sub,&layout);
    if(!r && (mp.drmFormatModifier!=0||desc.stride>UINT32_MAX/4||layout.rowPitch!=(VkDeviceSize)desc.stride*4))r=VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(!r){*fd=native->data[0];*format=0x34324241u;*stride=(uint32_t)layout.rowPitch;*offset=(uint32_t)layout.offset;*modifier=mp.drmFormatModifier;}
    switch_back(&scope);return r;
}
static VkResult mv_wsi_live_create_buffer(void *opaque,struct AHardwareBuffer *ahb,VkExtent2D extent,uint32_t format,int fd,
        uint32_t stride,uint32_t offset,uint64_t modifier,void **buffer) {
    struct mv_wsi_live_context *c=opaque;struct mv_wl_plane plane={.fd=fd,.index=0,.offset=offset,.stride=stride,.modifier=modifier};
    if(format!=0x34324241u)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(mv_wl_has_ahb_buffer(c->surface->dmabuf) && mv_ahb_send_socket) {
        int pair[2];if(mv_socketpair(pair)<0)return VK_ERROR_OUT_OF_HOST_MEMORY;
        struct mv_scope scope;
        if(!mv_enter(&scope)){mv_close(pair[0]);mv_close(pair[1]);return VK_ERROR_INITIALIZATION_FAILED;}
        int rc=mv_ahb_send_socket(ahb,pair[1]);
        switch_back(&scope);
        if(rc) {mv_close(pair[0]);mv_close(pair[1]);return VK_ERROR_OUT_OF_HOST_MEMORY;}
        *buffer=mv_wl_ahb_create_buffer(c->surface->dmabuf,extent.width,extent.height,format,pair[0]);
        mv_close(pair[0]);mv_close(pair[1]);
        return *buffer?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if(fd<0||!stride)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *buffer=mv_wl_dmabuf_create_buffer(c->surface->dmabuf,extent.width,extent.height,format,&plane,1);
    return *buffer?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void mv_wsi_live_destroy_buffer(void *opaque,void *buffer) {(void)opaque;mv_wl_buffer_destroy(buffer);}
static VkResult mv_wsi_live_present(void *opaque,uint64_t surface,uint64_t buffer,uint64_t serial,void *wl_buffer,int fence_fd) {
    (void)surface;(void)buffer;(void)serial;struct mv_wsi_live_context *c=opaque;
    return mv_wl_surface_present(c->surface->sync,c->surface->surface,wl_buffer,fence_fd)==0?VK_SUCCESS:VK_ERROR_DEVICE_LOST;
}

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

static void mv_vkDestroySurfaceKHR(VkInstance instance,VkSurfaceKHR surface,const VkAllocationCallbacks *allocator) {
    (void)allocator;
    struct mv_wayland_surface *s=mv_wsi_surface(instance,surface);
    if (s) {
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

static struct mv_wsi_live_context *mv_wsi_live(VkDevice device,VkSwapchainKHR swapchain) {
    if(!device||!swapchain)return 0;
    struct mv_wsi_swapchain *c=(void *)(uintptr_t)swapchain;
    struct mv_wsi_live_context *live=c->ops.context;
    return live&&live->device==(struct mv_device *)device?live:0;
}
static void mv_wsi_collect_releases(struct mv_wsi_swapchain *chain) {
    for(uint32_t j=0;j<chain->count;j++)if(mv_wl_buffer_released(chain->images[j].wl_buffer)) {
        mv_wl_buffer_clear_released(chain->images[j].wl_buffer);
        (void)mv_wsi_release(chain,chain->images[j].token,chain->images[j].serial);
    }
}
static VkResult mv_create_swapchain(VkDevice device,const VkSwapchainCreateInfoKHR *info,
        const VkAllocationCallbacks *allocator,VkSwapchainKHR *out) {
    if(!out)return VK_ERROR_INITIALIZATION_FAILED;*out=VK_NULL_HANDLE;
    struct mv_device *d=(void *)device;
    if(!d||!d->wsi_swapchain_enabled||!info||!mv_ahb_native_handle||!mv_ahb_describe||!mv_ahb_release)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(allocator&&(!allocator->pfnAllocation||!allocator->pfnReallocation||!allocator->pfnFree))return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_wayland_surface *surface=mv_wsi_surface((VkInstance)d->base.instance,info->surface);
    if(!surface)return VK_ERROR_SURFACE_LOST_KHR;
    if(info->oldSwapchain) {
        struct mv_wsi_live_context *old_live=mv_wsi_live(device,info->oldSwapchain);
        if(!old_live||old_live->surface!=surface)return VK_ERROR_OUT_OF_DATE_KHR;
    }
    if(!surface->sync)surface->sync=mv_wl_sync_surface_create(surface->dmabuf,surface->surface);
    if(!surface->sync)return VK_ERROR_FEATURE_NOT_PRESENT;
    struct mv_wsi_live_context *c=mv_alloc(sizeof(*c));if(!c)return VK_ERROR_OUT_OF_HOST_MEMORY;memset(c,0,sizeof(*c));
    c->device=d;c->surface=surface;
    struct mv_scope scope;
    if(!mv_enter(&scope)){mv_free(c);return VK_ERROR_INITIALIZATION_FAILED;}
    c->create_image=(PFN_vkCreateImage)d->gdpa(d->base.android,"vkCreateImage");
    c->get_requirements=(PFN_vkGetImageMemoryRequirements)d->gdpa(d->base.android,"vkGetImageMemoryRequirements");
    c->allocate_memory=(PFN_vkAllocateMemory)d->gdpa(d->base.android,"vkAllocateMemory");
    c->bind_image=(PFN_vkBindImageMemory)d->gdpa(d->base.android,"vkBindImageMemory");
    c->destroy_image=(PFN_vkDestroyImage)d->gdpa(d->base.android,"vkDestroyImage");
    c->free_memory=(PFN_vkFreeMemory)d->gdpa(d->base.android,"vkFreeMemory");
    c->get_ahb=(PFN_vkGetMemoryAndroidHardwareBufferANDROID)d->gdpa(d->base.android,"vkGetMemoryAndroidHardwareBufferANDROID");
    c->get_modifier=(PFN_vkGetImageDrmFormatModifierPropertiesEXT)d->gdpa(d->base.android,"vkGetImageDrmFormatModifierPropertiesEXT");
    c->get_layout=(PFN_vkGetImageSubresourceLayout)d->gdpa(d->base.android,"vkGetImageSubresourceLayout");
    switch_back(&scope);
    if(!c->create_image||!c->get_requirements||!c->allocate_memory||!c->bind_image||!c->destroy_image||
       !c->free_memory||!c->get_ahb||!c->get_modifier||!c->get_layout){mv_free(c);return VK_ERROR_EXTENSION_NOT_PRESENT;}
    VkExtent2D previous_extent=surface->state.extent;
    uint64_t previous_generation=surface->state.generation;
    mv_wsi_surface_resize(&surface->state,info->imageExtent);
    struct mv_wsi_ops ops={.context=c,.allocate=mv_wsi_live_allocate,.release=mv_wsi_live_release,
        .destroy=mv_wsi_live_destroy,.export_dmabuf=mv_wsi_live_export,.create_buffer=mv_wsi_live_create_buffer,
        .destroy_buffer=mv_wsi_live_destroy_buffer,.present=mv_wsi_live_present};
    struct mv_wsi_swapchain *chain=0;VkResult r=mv_wsi_create(&surface->state,info,&ops,&chain);
    if(r){surface->state.extent=previous_extent;surface->state.generation=previous_generation;mv_free(c);return r;}
    *out=(VkSwapchainKHR)(uintptr_t)chain;return VK_SUCCESS;
}
static VkResult mv_vkCreateSwapchainKHR(VkDevice device,const VkSwapchainCreateInfoKHR *info,
        const VkAllocationCallbacks *allocator,VkSwapchainKHR *out) {
    VkResult r=mv_create_swapchain(device,info,allocator,out);
    mv_trace_result("maton-vulkan: create swapchain",r,info?info->minImageCount:0);
    return r;
}
static void mv_vkDestroySwapchainKHR(VkDevice device,VkSwapchainKHR swapchain,const VkAllocationCallbacks *allocator) {
    (void)allocator;struct mv_wsi_live_context *live=mv_wsi_live(device,swapchain);if(!live)return;
    struct mv_wsi_swapchain *c=(void *)(uintptr_t)swapchain;
    for(;;) {
        mv_wsi_collect_releases(c);int pending=0;
        for(uint32_t j=0;j<c->count;j++)pending|=c->images[j].state==2;
        if(!pending||mv_wl_dmabuf_dispatch_one(live->surface->dmabuf)<0)break;
    }
    if(mv_wsi_destroy(c)==VK_SUCCESS)mv_free(live);
}
static VkResult mv_vkGetSwapchainImagesKHR(VkDevice device,VkSwapchainKHR swapchain,uint32_t *count,VkImage *images) {
    struct mv_wsi_live_context *live=mv_wsi_live(device,swapchain);if(!live)return VK_ERROR_OUT_OF_DATE_KHR;
    return mv_wsi_images((void *)(uintptr_t)swapchain,count,images);
}
static VkResult mv_vkAcquireNextImageKHR(VkDevice device,VkSwapchainKHR swapchain,uint64_t timeout,
        VkSemaphore semaphore,VkFence fence,uint32_t *index) {
    struct mv_wsi_live_context *live=mv_wsi_live(device,swapchain);if(!live)return VK_ERROR_OUT_OF_DATE_KHR;
    struct mv_wsi_swapchain *chain=(void *)(uintptr_t)swapchain;
    for(;;) {
        (void)mv_wl_dmabuf_dispatch(live->surface->dmabuf);
        mv_wsi_collect_releases(chain);
        VkResult r=mv_wsi_acquire(chain,index);
        if(r!=VK_NOT_READY) {
            if(r!=VK_SUCCESS)return r;
            struct mv_device *d=(void *)device;struct mv_handle *q=d->children;
            while(q&&(q->pool||q->device!=d))q=q->next;
            if(!q){mv_wsi_cancel_acquire(chain,*index);return VK_ERROR_INITIALIZATION_FAILED;}
            VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.signalSemaphoreCount=semaphore?1u:0u,
                .pSignalSemaphores=semaphore?&semaphore:NULL};struct mv_scope scope;
            if(!mv_enter(&scope)){mv_wsi_cancel_acquire(chain,*index);return VK_ERROR_INITIALIZATION_FAILED;}
            VkResult sr=d->dispatch.vkQueueSubmit(q->android,1,&si,fence);switch_back(&scope);
            if(sr)mv_wsi_cancel_acquire(chain,*index);return sr;
        }
        if(!timeout)return VK_NOT_READY;
        if(mv_wl_dmabuf_dispatch_one(live->surface->dmabuf)<0)return VK_ERROR_SURFACE_LOST_KHR;
    }
}
static VkResult mv_vkAcquireNextImage2KHR(VkDevice device,const VkAcquireNextImageInfoKHR *info,uint32_t *index) {
    if(!info||info->pNext)return VK_ERROR_FEATURE_NOT_PRESENT;
    return mv_vkAcquireNextImageKHR(device,info->swapchain,info->timeout,info->semaphore,info->fence,index);
}
static VkResult mv_vkQueuePresentKHR(VkQueue queue,const VkPresentInfoKHR *info) {
    if(!queue||!info||info->pNext||info->swapchainCount!=1||!info->pSwapchains||!info->pImageIndices)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct mv_handle *qh=(void *)queue;struct mv_device *d=qh->device;
    if(!d||!d->wsi_swapchain_enabled||!d->wsi_sync_fd_enabled)return VK_ERROR_EXTENSION_NOT_PRESENT;
    struct mv_wsi_live_context *live=mv_wsi_live((VkDevice)d,(VkSwapchainKHR)info->pSwapchains[0]);
    if(!live)return VK_ERROR_OUT_OF_DATE_KHR;
    struct mv_wsi_swapchain *chain=(void *)(uintptr_t)info->pSwapchains[0];
    struct mv_scope scope;if(!mv_enter(&scope))return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateSemaphore create=(PFN_vkCreateSemaphore)d->gdpa(d->base.android,"vkCreateSemaphore");
    PFN_vkGetSemaphoreFdKHR get_fd=(PFN_vkGetSemaphoreFdKHR)d->gdpa(d->base.android,"vkGetSemaphoreFdKHR");
    PFN_vkDestroySemaphore destroy=(PFN_vkDestroySemaphore)d->gdpa(d->base.android,"vkDestroySemaphore");
    if(!create||!get_fd||!destroy){switch_back(&scope);return VK_ERROR_EXTENSION_NOT_PRESENT;}
    VkExportSemaphoreCreateInfo export={.sType=VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkSemaphoreCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,.pNext=&export};
    VkSemaphore signal=VK_NULL_HANDLE;VkResult r=create(d->base.android,&ci,NULL,&signal);
    VkPipelineStageFlags *stages=NULL;
    if(!r&&info->waitSemaphoreCount){stages=mv_alloc((size_t)info->waitSemaphoreCount*sizeof(*stages));if(!stages)r=VK_ERROR_OUT_OF_HOST_MEMORY;else for(uint32_t j=0;j<info->waitSemaphoreCount;j++)stages[j]=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;}
    if(!r) {
        VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=info->waitSemaphoreCount,
            .pWaitSemaphores=info->pWaitSemaphores,.pWaitDstStageMask=stages,.signalSemaphoreCount=1,.pSignalSemaphores=&signal};
        r=d->dispatch.vkQueueSubmit(qh->android,1,&submit,VK_NULL_HANDLE);
    }
    int fd=-1;if(!r){VkSemaphoreGetFdInfoKHR gi={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore=signal,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};r=get_fd(d->base.android,&gi,&fd);}
    if(signal)destroy(d->base.android,signal,NULL);switch_back(&scope);mv_free(stages);
    if(r){if(fd>=0)mv_close(fd);return r;}
    r=mv_wsi_present(chain,info->pImageIndices[0],fd);
    mv_close(fd);
    if(info->pResults)info->pResults[0]=r;
    return r;
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
#undef MV_WSI_DEVICE_PROC
    return 0;
}
