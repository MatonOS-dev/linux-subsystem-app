/* Emulated VK_KHR_external_memory_fd + VK_EXT_external_memory_dma_buf.
 * Android has neither. The fds are the AHB's own first native-handle fd (a
 * dma-buf on GKI devices); Xwayland's DRI3 exports hand out the same. An
 * imported fd is resolved back to its AHardwareBuffer by identity over
 * Xwayland's AHB socket and imported with
 * VK_ANDROID_external_memory_android_hardware_buffer. Export allocates an
 * exportable AHB; vkGetMemoryFdKHR returns a dup of its native-handle fd,
 * registered with Xwayland (MAHR) until the memory is freed, so DRI3
 * PixmapFromBuffer and later imports resolve it. */
#define MV_DMABUF VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
#define MV_AHB VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID
static int mv_dmabuf_name(const char *name) {
    return !strcmp(name,VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)||!strcmp(name,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
}
/* Backed by the same resident extensions as WSI, plus the AHB socket receiver. */
static int mv_dmabuf_supported(const VkExtensionProperties *all,uint32_t count) {
    return mv_ahb_recv_socket&&mv_ahb_release&&
        mv_wsi_has_requirements(mv_wsi_x11_requirements,MV_WSI_REQUIREMENT_COUNT,all,count);
}
static VkBaseOutStructure *mv_chain_find(VkBaseOutStructure *p,VkStructureType type) {
    while(p&&p->sType!=type)p=p->pNext;return p;
}
static VkPhysicalDeviceImageFormatInfo2 mv_dmabuf_image_info(const VkPhysicalDeviceImageFormatInfo2 *info,VkBaseOutStructure *chain) {
    VkPhysicalDeviceImageFormatInfo2 copy=*info;copy.pNext=chain;
    VkPhysicalDeviceExternalImageFormatInfo *e=(void *)mv_chain_find(chain,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
    if(e&&e->handleType==MV_DMABUF){e->handleType=MV_AHB;if(copy.tiling==VK_IMAGE_TILING_LINEAR)copy.tiling=VK_IMAGE_TILING_OPTIMAL;}
    return copy;
}
static VkResult mv_vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physical,const VkPhysicalDeviceImageFormatInfo2 *info,VkImageFormatProperties2 *out) {
    if(!mv_valid_VkPhysicalDeviceImageFormatInfo2(info))return VK_ERROR_FEATURE_NOT_PRESENT;
    struct mv_handle *h=(void *)physical;
    const VkPhysicalDeviceExternalImageFormatInfo *in=0;
    for(const VkBaseInStructure *p=info->pNext;p;p=p->pNext)if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO)in=(const void *)p;
    int dmabuf=in&&in->handleType==MV_DMABUF;
    if(dmabuf&&info->tiling!=VK_IMAGE_TILING_LINEAR&&info->tiling!=VK_IMAGE_TILING_OPTIMAL)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkBaseOutStructure *chain=0;
    VkResult r=mv_chain_copy(info->pNext,&chain);if(r)return r;
    VkPhysicalDeviceImageFormatInfo2 copy=mv_dmabuf_image_info(info,chain);
    struct mv_scope s;if(!mv_enter(&s)){mv_chain_free(chain);return VK_ERROR_INITIALIZATION_FAILED;}
    PFN_vkGetPhysicalDeviceImageFormatProperties2 f=(void *)mv_gipa(h->instance->base.android,"vkGetPhysicalDeviceImageFormatProperties2");
    r=f?f(h->android,&copy,out):VK_ERROR_INITIALIZATION_FAILED;
    switch_back(&s);mv_chain_free(chain);
    if(r||!dmabuf)return r;
    VkExternalImageFormatProperties *e=(void *)mv_chain_find((VkBaseOutStructure *)out->pNext,VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
    if(e) {
        VkExternalMemoryProperties *m=&e->externalMemoryProperties;
        if(!(m->externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))return VK_ERROR_FORMAT_NOT_SUPPORTED;
        /* AHB images are always dedicated; export follows the AHB's. */
        m->externalMemoryFeatures=VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT|VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT|
            (m->externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT);
        m->exportFromImportedHandleTypes=0;m->compatibleHandleTypes=MV_DMABUF;
    }
    return VK_SUCCESS;
}
/* Images converted to AHB-backed ones. Vulkan forbids memory-requirement
 * queries on an AHB image before it is bound (VUID-VkImageMemoryRequirementsInfo2-image-01897),
 * yet dma-buf importers (Zink) ask first. The answer comes from a twin image
 * with the same parameters and no external handle type; the AHB import in
 * vkAllocateMemory then takes size and memory type from the AHB itself. */
/* linear: the app asked for LINEAR tiling. The backing is an AHB with its
 * own layout (OPTIMAL to the driver), so the resident driver must never see
 * a layout query for it (VUID-vkGetImageSubresourceLayout-image-07790); we
 * answer with the bound AHB's own row stride (pitch, set at bind). */
struct mv_ahb_image { struct mv_ahb_image *next; VkImage image; VkMemoryRequirements req; int linear; uint32_t width,height,texel; VkDeviceSize pitch; };
static uint32_t mv_dmabuf_texel(VkFormat f) {
    switch(f) {
    case VK_FORMAT_R8_UNORM:return 1;
    case VK_FORMAT_R8G8_UNORM:case VK_FORMAT_R5G6B5_UNORM_PACK16:case VK_FORMAT_B5G6R5_UNORM_PACK16:return 2;
    case VK_FORMAT_R16G16B16A16_SFLOAT:case VK_FORMAT_R16G16B16A16_UNORM:return 8;
    default:return (f>=VK_FORMAT_R8G8B8A8_UNORM&&f<=VK_FORMAT_A2B10G10R10_SINT_PACK32)?4:0;
    }
}
static int mv_ahb_image_linear(struct mv_device *d,VkImage image,struct mv_ahb_image *out) {
    int found=0;
    while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
    for(struct mv_ahb_image *e=d->ahb_images;e;e=e->next)if(e->image==image){found=e->linear;*out=*e;break;}
    __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);
    return found;
}
static int mv_ahb_image_find(struct mv_device *d,VkImage image,VkMemoryRequirements *req) {
    int found=0;
    while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
    for(struct mv_ahb_image *e=d->ahb_images;e;e=e->next)if(e->image==image){*req=e->req;found=1;break;}
    __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);
    return found;
}
static void mv_ahb_images_free(struct mv_device *d) {
    while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
    struct mv_ahb_image *e=d->ahb_images;d->ahb_images=0;
    __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);
    while(e){struct mv_ahb_image *next=e->next;mv_free(e);e=next;}
}
static VkResult mv_vkCreateImage(VkDevice device,const VkImageCreateInfo *info,const VkAllocationCallbacks *a,VkImage *out) {
    (void)a;if(!mv_valid_VkImageCreateInfo(info))return VK_ERROR_FEATURE_NOT_PRESENT;
    struct mv_device *d=(void *)device;
    VkBaseOutStructure *chain=0;VkResult r=mv_chain_copy(info->pNext,&chain);if(r)return r;
    VkImageCreateInfo copy=*info;copy.pNext=chain;
    VkExternalMemoryImageCreateInfo *e=(void *)mv_chain_find(chain,VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
    struct mv_ahb_image *entry=0;
    if(e&&(e->handleTypes&MV_DMABUF)) {
        if(!d->dmabuf_enabled){mv_chain_free(chain);return VK_ERROR_FEATURE_NOT_PRESENT;}
        /* The AHB carries its own layout; Android requires OPTIMAL tiling for it. */
        e->handleTypes=(e->handleTypes&~MV_DMABUF)|MV_AHB;
        int linear=copy.tiling==VK_IMAGE_TILING_LINEAR;
        if(linear)copy.tiling=VK_IMAGE_TILING_OPTIMAL;
        uint32_t texel=mv_dmabuf_texel(copy.format);
        if(linear&&(!texel||copy.mipLevels!=1||copy.arrayLayers!=1||copy.extent.depth!=1)){
            mv_loud("maton-vulkan: LINEAR dma-buf image of unsupported shape/format",copy.format);mv_chain_free(chain);return VK_ERROR_FORMAT_NOT_SUPPORTED;}
        entry=mv_alloc(sizeof(*entry));
        if(!entry){mv_chain_free(chain);return VK_ERROR_OUT_OF_HOST_MEMORY;}
        memset(entry,0,sizeof(*entry));entry->linear=linear;entry->width=copy.extent.width;entry->height=copy.extent.height;entry->texel=texel;
    }
    struct mv_scope s;if(!mv_enter(&s)){mv_free(entry);mv_chain_free(chain);return VK_ERROR_INITIALIZATION_FAILED;}
    r=d->dispatch.vkCreateImage(d->base.android,&copy,0,out);
    if(!r&&entry) {
        /* Twin: same chain minus the external-memory struct. */
        VkBaseOutStructure **link=(VkBaseOutStructure **)&copy.pNext;
        while(*link!=(VkBaseOutStructure *)e)link=&(*link)->pNext;
        *link=(VkBaseOutStructure *)e->pNext;
        VkImage twin=VK_NULL_HANDLE;
        r=d->dispatch.vkCreateImage(d->base.android,&copy,0,&twin);
        *link=(VkBaseOutStructure *)e; /* chain_free walks the full chain */
        if(!r){d->dispatch.vkGetImageMemoryRequirements(d->base.android,twin,&entry->req);d->dispatch.vkDestroyImage(d->base.android,twin,0);}
        else{d->dispatch.vkDestroyImage(d->base.android,*out,0);*out=VK_NULL_HANDLE;}
    }
    switch_back(&s);mv_chain_free(chain);
    if(r){if(e)mv_trace_result("maton-vulkan: dma-buf image create failed",r,copy.format);mv_free(entry);return r;}
    if(entry) {
        entry->image=*out;
        while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
        entry->next=d->ahb_images;d->ahb_images=entry;
        __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);
    }
    return VK_SUCCESS;
}
static void mv_vkDestroyImage(VkDevice device,VkImage image,const VkAllocationCallbacks *a) {
    (void)a;struct mv_device *d=(void *)device;
    if(image) {
        struct mv_ahb_image *drop=0,**link;
        while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
        for(link=&d->ahb_images;*link;link=&(*link)->next)if((*link)->image==image){drop=*link;*link=drop->next;break;}
        __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);
        mv_free(drop);
    }
    struct mv_scope s;if(!mv_enter(&s))return;
    d->dispatch.vkDestroyImage(d->base.android,image,0);switch_back(&s);
}
static void mv_vkGetImageSubresourceLayout(VkDevice device,VkImage image,const VkImageSubresource *sub,VkSubresourceLayout *out) {
    struct mv_device *d=(void *)device;struct mv_ahb_image e;
    if(mv_ahb_image_linear(d,image,&e)) {
        VkDeviceSize pitch=e.pitch;
        if(!pitch)mv_loud("maton-vulkan: layout query on an unbound LINEAR dma-buf image; no AHB stride yet",e.width);
        *out=(VkSubresourceLayout){.offset=0,.size=pitch*e.height,.rowPitch=pitch,.arrayPitch=pitch*e.height,.depthPitch=pitch*e.height};
        return;
    }
    struct mv_scope s;if(!mv_enter(&s)){*out=(VkSubresourceLayout){0};return;}
    d->dispatch.vkGetImageSubresourceLayout(d->base.android,image,sub,out);switch_back(&s);
}
/* LINEAR dma-buf images report the real layout of the AHB they are bound to. */
static void mv_ahb_image_bound(struct mv_device *d,VkImage image,VkDeviceMemory memory) {
    struct mv_ahb_image e;
    if(!mv_ahb_image_linear(d,image,&e))return;
    struct AHardwareBuffer *ahb=0;struct mv_ahb_desc desc={0};VkResult r=VK_ERROR_EXTENSION_NOT_PRESENT;
    VkMemoryGetAndroidHardwareBufferInfoANDROID get={.sType=VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,.memory=memory};
    struct mv_scope s;if(!mv_enter(&s)){mv_loud("maton-vulkan: bind: cannot enter Android scope for AHB stride",0);return;}
    if(d->ahb_export&&mv_ahb_describe)r=d->ahb_export(d->base.android,&get,&ahb);
    if(!r&&ahb){mv_ahb_describe(ahb,&desc);mv_ahb_release(ahb);}
    switch_back(&s);
    if(r||!ahb||!desc.stride){mv_trace_result("maton-vulkan: LINEAR dma-buf image bound to memory without a describable AHB",r,desc.stride);return;}
    if(desc.width!=e.width||desc.height!=e.height)mv_loud("maton-vulkan: LINEAR dma-buf image size differs from its AHB",desc.width);
    while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
    for(struct mv_ahb_image *x=d->ahb_images;x;x=x->next)if(x->image==image){x->pitch=(VkDeviceSize)desc.stride*x->texel;break;}
    __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);
}
static VkResult mv_vkBindImageMemory(VkDevice device,VkImage image,VkDeviceMemory memory,VkDeviceSize offset) {
    struct mv_device *d=(void *)device;
    struct mv_scope s;if(!mv_enter(&s))return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=d->dispatch.vkBindImageMemory(d->base.android,image,memory,offset);switch_back(&s);
    if(!r)mv_ahb_image_bound(d,image,memory);
    return r;
}
static VkResult mv_vkBindImageMemory2(VkDevice device,uint32_t n,const VkBindImageMemoryInfo *infos) {
    struct mv_device *d=(void *)device;
    for(uint32_t i=0;i<n;i++)if(!mv_chain_ok(infos[i].pNext))return VK_ERROR_FEATURE_NOT_PRESENT;
    struct mv_scope s;if(!mv_enter(&s))return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=d->dispatch.vkBindImageMemory2(d->base.android,n,infos);switch_back(&s);
    if(!r)for(uint32_t i=0;i<n;i++)mv_ahb_image_bound(d,infos[i].image,infos[i].memory);
    return r;
}
static void mv_vkGetImageMemoryRequirements(VkDevice device,VkImage image,VkMemoryRequirements *out) {
    struct mv_device *d=(void *)device;
    if(mv_ahb_image_find(d,image,out))return;
    struct mv_scope s;if(!mv_enter(&s)){*out=(VkMemoryRequirements){0};return;}
    d->dispatch.vkGetImageMemoryRequirements(d->base.android,image,out);switch_back(&s);
}
static void mv_vkGetImageMemoryRequirements2(VkDevice device,const VkImageMemoryRequirementsInfo2 *info,VkMemoryRequirements2 *out) {
    struct mv_device *d=(void *)device;
    /* Planar plane queries (VkImagePlaneMemoryRequirementsInfo) do not apply: AHB dma-buf imports are single-plane here. */
    if(info&&!info->pNext&&mv_ahb_image_find(d,info->image,&out->memoryRequirements)) {
        VkMemoryDedicatedRequirements *ded=(void *)mv_chain_find((VkBaseOutStructure *)out->pNext,VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
        if(ded)ded->prefersDedicatedAllocation=ded->requiresDedicatedAllocation=VK_TRUE;
        return;
    }
    if(!info){out->memoryRequirements=(VkMemoryRequirements){0};return;}
    struct mv_scope s;if(!mv_enter(&s)){out->memoryRequirements=(VkMemoryRequirements){0};return;}
    d->dispatch.vkGetImageMemoryRequirements2(d->base.android,info,out);switch_back(&s);
}
/* Resolve an exported AHB fd to an AHB reference (released by the caller). */
static struct AHardwareBuffer *mv_dmabuf_ahb(int fd) {
    int sock=mv_x11_ahb_lookup(fd);
    if(sock<0){mv_trace_result("maton-vulkan: dma-buf lookup connect failed (stage*1000+errno)",mv_x11_last_error,0);return 0;}
    struct AHardwareBuffer *ahb=0;int rc=-1;struct mv_scope s;
    if(mv_enter(&s)){rc=mv_ahb_recv_socket(sock,&ahb);switch_back(&s);}
    mv_close(sock);
    if(rc||!ahb){mv_trace_result("maton-vulkan: dma-buf fd is not a live Xwayland DRI3 export",rc,0);return 0;}
    return ahb;
}
static VkResult mv_vkAllocateMemory(VkDevice device,const VkMemoryAllocateInfo *info,const VkAllocationCallbacks *a,VkDeviceMemory *out) {
    (void)a;if(!mv_valid_VkMemoryAllocateInfo(info))return VK_ERROR_FEATURE_NOT_PRESENT;
    struct mv_device *d=(void *)device;
    VkBaseOutStructure *chain=0;VkResult r=mv_chain_copy(info->pNext,&chain);if(r)return r;
    VkMemoryAllocateInfo copy=*info;copy.pNext=chain;
    VkExportMemoryAllocateInfo *ex=(void *)mv_chain_find(chain,VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
    if(ex&&(ex->handleTypes&MV_DMABUF)) {
        if(!d->dmabuf_enabled||!d->ahb_export){mv_trace_result("maton-vulkan: dma-buf export without the emulated extension",ex->handleTypes,0);mv_chain_free(chain);return VK_ERROR_INVALID_EXTERNAL_HANDLE;}
        /* Backed by an exportable AHB; vkGetMemoryFdKHR hands out its native-handle fd. */
        ex->handleTypes=(ex->handleTypes&~MV_DMABUF)|MV_AHB;
        /* Android sizes AHB exports itself (dedicated images need size 0). */
        if(mv_chain_find(chain,VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO))copy.allocationSize=0;
    }
    VkImportMemoryFdInfoKHR *fdi=(void *)mv_chain_find(chain,VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
    struct AHardwareBuffer *ahb=0;int fd=-1;
    if(fdi) {
        if(!d->dmabuf_enabled||fdi->handleType!=MV_DMABUF) {
            mv_trace_result("maton-vulkan: fd import of unsupported handle type",fdi->handleType,0);
            mv_chain_free(chain);return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        if(!mv_chain_find(chain,VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO)) {
            mv_trace_result("maton-vulkan: dma-buf import without dedicated image",0,0);
            mv_chain_free(chain);return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        fd=fdi->fd;ahb=mv_dmabuf_ahb(fd);
        if(!ahb){mv_chain_free(chain);return VK_ERROR_INVALID_EXTERNAL_HANDLE;}
        VkImportAndroidHardwareBufferInfoANDROID *imp=mv_alloc(sizeof(*imp));
        if(!imp){mv_chain_free(chain);struct mv_scope s;if(mv_enter(&s)){mv_ahb_release(ahb);switch_back(&s);}return VK_ERROR_OUT_OF_HOST_MEMORY;}
        *imp=(VkImportAndroidHardwareBufferInfoANDROID){.sType=VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,.pNext=fdi->pNext,.buffer=ahb};
        VkBaseOutStructure **link=(VkBaseOutStructure **)&copy.pNext;
        while(*link!=(VkBaseOutStructure *)fdi)link=&(*link)->pNext;
        *link=(VkBaseOutStructure *)imp;mv_free(fdi);
    }
    struct mv_scope s;if(!mv_enter(&s)){mv_chain_free((VkBaseOutStructure *)copy.pNext);return VK_ERROR_INITIALIZATION_FAILED;}
    if(ahb) {
        VkAndroidHardwareBufferPropertiesANDROID hp={.sType=VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
        r=d->ahb_properties?d->ahb_properties(d->base.android,ahb,&hp):VK_ERROR_INITIALIZATION_FAILED;
        if(!r&&!hp.memoryTypeBits)r=VK_ERROR_INVALID_EXTERNAL_HANDLE;
        if(!r) {
            copy.allocationSize=hp.allocationSize;
            if(!(hp.memoryTypeBits&(1u<<copy.memoryTypeIndex))) {
                uint32_t type=0;while(!(hp.memoryTypeBits&(1u<<type)))type++;
                copy.memoryTypeIndex=type;
            }
        }
    }
    if(!r)r=d->dispatch.vkAllocateMemory(d->base.android,&copy,0,out);
    /* Vulkan holds its own AHB reference after a successful import. */
    if(ahb)mv_ahb_release(ahb);
    switch_back(&s);mv_chain_free((VkBaseOutStructure *)copy.pNext);
    if(ahb) {
        if(r)mv_trace_result("maton-vulkan: dma-buf AHB import failed",r,copy.memoryTypeIndex);
        else mv_close(fd); /* a successful fd import owns the fd */
    }
    return r;
}
static VkResult mv_vkGetMemoryFdPropertiesKHR(VkDevice device,VkExternalMemoryHandleTypeFlagBits type,int fd,VkMemoryFdPropertiesKHR *out) {
    struct mv_device *d=(void *)device;
    if(!d->dmabuf_enabled||type!=MV_DMABUF){mv_trace_result("maton-vulkan: fd properties for unsupported handle type",type,0);return VK_ERROR_INVALID_EXTERNAL_HANDLE;}
    struct AHardwareBuffer *ahb=mv_dmabuf_ahb(fd);if(!ahb)return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    VkAndroidHardwareBufferPropertiesANDROID hp={.sType=VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    struct mv_scope s;if(!mv_enter(&s))return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=d->ahb_properties?d->ahb_properties(d->base.android,ahb,&hp):VK_ERROR_INITIALIZATION_FAILED;
    mv_ahb_release(ahb);switch_back(&s);
    if(!r)out->memoryTypeBits=hp.memoryTypeBits;
    else mv_trace_result("maton-vulkan: dma-buf AHB properties failed",r,0);
    return r;
}
/* Emulated entry points exist even though the resident driver lacks them. */
static int mv_dmabuf_emulated(const char *name) {
    return !strcmp(name,"vkGetMemoryFdKHR")||!strcmp(name,"vkGetMemoryFdPropertiesKHR");
}
/* One Xwayland registration per exported memory; fd is our copy of the export,
 * dup'd for repeat vkGetMemoryFdKHR calls (Zink asks every submit for implicit sync). */
struct mv_dmabuf_reg { struct mv_dmabuf_reg *next; VkDeviceMemory memory; int socket,fd; };
/* Drop the Xwayland registrations of one memory (or all, at device teardown). */
static void mv_dmabuf_unregister(struct mv_device *d,VkDeviceMemory memory,int all) {
    struct mv_dmabuf_reg *drop=0,**link;
    while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
    for(link=&d->dmabuf_regs;*link;) {
        struct mv_dmabuf_reg *r=*link;
        if(all||r->memory==memory){*link=r->next;r->next=drop;drop=r;}else link=&r->next;
    }
    __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);
    while(drop){struct mv_dmabuf_reg *next=drop->next;mv_x11_ahb_unregister(drop->socket);mv_close(drop->fd);mv_free(drop);drop=next;}
}
static void mv_vkFreeMemory(VkDevice device,VkDeviceMemory memory,const VkAllocationCallbacks *a) {
    (void)a;struct mv_device *d=(void *)device;
    if(memory&&d->dmabuf_enabled)mv_dmabuf_unregister(d,memory,0);
    struct mv_scope s;if(!mv_enter(&s))return;
    d->dispatch.vkFreeMemory(d->base.android,memory,0);switch_back(&s);
}
static VkResult mv_vkGetMemoryFdKHR(VkDevice device,const VkMemoryGetFdInfoKHR *info,int *fd) {
    struct mv_device *d=(void *)device;*fd=-1;
    if(!info||info->handleType!=MV_DMABUF||!d->dmabuf_enabled||!d->ahb_export) {
        mv_trace_result("maton-vulkan: vkGetMemoryFdKHR: unsupported handle type",info?info->handleType:0,0);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    int again=-2;
    while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
    for(struct mv_dmabuf_reg *e=d->dmabuf_regs;e;e=e->next)if(e->memory==info->memory){again=mv_dup_cloexec(e->fd);break;}
    __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);
    if(again>=0){*fd=again;return VK_SUCCESS;}
    if(again!=-2){mv_trace_result("maton-vulkan: dma-buf re-export dup failed",again,0);return VK_ERROR_TOO_MANY_OBJECTS;}
    struct mv_dmabuf_reg *reg=mv_alloc(sizeof(*reg));if(!reg)return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct AHardwareBuffer *ahb=0;struct mv_ahb_desc desc={0};int pair[2]={-1,-1},out=-1,sent=-1;
    VkMemoryGetAndroidHardwareBufferInfoANDROID get={.sType=VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,.memory=info->memory};
    struct mv_scope s;if(!mv_enter(&s)){mv_free(reg);return VK_ERROR_INITIALIZATION_FAILED;}
    VkResult r=d->ahb_export(d->base.android,&get,&ahb);
    if(!r&&ahb) {
        mv_ahb_describe(ahb,&desc);
        /* The native handle is valid while this reference is held. */
        const struct { int version,numFds,numInts; int data[]; } *h=mv_ahb_native_handle?mv_ahb_native_handle(ahb):0;
        out=h&&h->numFds>0?mv_dup_cloexec(h->data[0]):-1;
        if(out<0)mv_trace_result("maton-vulkan: dma-buf export: AHB native handle has no fd (dup result, numFds)",out,h?h->numFds:-1);
        int sp=out<0?0:mv_socketpair(pair);
        if(sp)mv_trace_result("maton-vulkan: dma-buf export socketpair failed",sp,0);
        else sent=mv_ahb_send_socket(ahb,pair[1]);
        mv_ahb_release(ahb); /* the transport carries its own reference */
    }
    switch_back(&s);
    if(r||!ahb){mv_trace_result("maton-vulkan: dma-buf export: memory has no AHB",r,0);mv_free(reg);return r?r:VK_ERROR_INVALID_EXTERNAL_HANDLE;}
    if(out<0){mv_free(reg);return VK_ERROR_INVALID_EXTERNAL_HANDLE;}
    if(sent){
        if(pair[0]>=0)mv_trace_result("maton-vulkan: dma-buf export AHB send failed",sent,0);
        if(pair[0]>=0){mv_close(pair[0]);mv_close(pair[1]);}
        mv_close(out);mv_free(reg);return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    mv_close(pair[1]);
    reg->socket=mv_x11_ahb_register(out,pair[0]);
    mv_close(pair[0]);
    if(reg->socket<0) {
        mv_trace_result("maton-vulkan: dma-buf export register failed (stage*1000+errno)",mv_x11_last_error,0);
        mv_close(out);mv_free(reg);return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    reg->fd=mv_dup_cloexec(out);
    if(reg->fd<0) {
        mv_trace_result("maton-vulkan: dma-buf export keep-fd dup failed",reg->fd,0);
        mv_x11_ahb_unregister(reg->socket);mv_close(out);mv_free(reg);return VK_ERROR_TOO_MANY_OBJECTS;
    }
    reg->memory=info->memory;
    while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
    reg->next=d->dmabuf_regs;d->dmabuf_regs=reg;
    __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);
    mv_trace_result("maton-vulkan: dma-buf exported native-handle fd, AHB format",out,desc.format);
    *fd=out;return VK_SUCCESS;
}
