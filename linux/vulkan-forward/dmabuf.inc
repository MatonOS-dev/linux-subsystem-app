/* Emulated VK_KHR_external_memory_fd + VK_EXT_external_memory_dma_buf.
 * Android has neither. Linux clients receive DRI3 stand-in fds from Xwayland
 * (memfds, never gralloc fds); an imported stand-in is resolved back to its
 * AHardwareBuffer over Xwayland's AHB socket and imported with
 * VK_ANDROID_external_memory_android_hardware_buffer. Export allocates an
 * exportable AHB; vkGetMemoryFdKHR returns a fresh memfd stand-in registered
 * with Xwayland (MAHR) until the memory is freed, so DRI3 PixmapFromBuffer and
 * later imports resolve it. A stand-in is not CPU-mappable pixel memory. */
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
static VkResult mv_vkCreateImage(VkDevice device,const VkImageCreateInfo *info,const VkAllocationCallbacks *a,VkImage *out) {
    (void)a;if(!mv_valid_VkImageCreateInfo(info))return VK_ERROR_FEATURE_NOT_PRESENT;
    struct mv_device *d=(void *)device;
    VkBaseOutStructure *chain=0;VkResult r=mv_chain_copy(info->pNext,&chain);if(r)return r;
    VkImageCreateInfo copy=*info;copy.pNext=chain;
    VkExternalMemoryImageCreateInfo *e=(void *)mv_chain_find(chain,VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
    if(e&&(e->handleTypes&MV_DMABUF)) {
        if(!d->dmabuf_enabled){mv_chain_free(chain);return VK_ERROR_FEATURE_NOT_PRESENT;}
        /* The AHB carries its own layout; Android requires OPTIMAL tiling for it. */
        e->handleTypes=(e->handleTypes&~MV_DMABUF)|MV_AHB;
        if(copy.tiling==VK_IMAGE_TILING_LINEAR)copy.tiling=VK_IMAGE_TILING_OPTIMAL;
    }
    struct mv_scope s;if(!mv_enter(&s)){mv_chain_free(chain);return VK_ERROR_INITIALIZATION_FAILED;}
    r=d->dispatch.vkCreateImage(d->base.android,&copy,0,out);
    switch_back(&s);mv_chain_free(chain);
    if(r&&e)mv_trace_result("maton-vulkan: dma-buf image create failed",r,copy.format);
    return r;
}
/* Resolve a stand-in fd to an AHB reference (released by the caller). */
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
        /* Backed by an exportable AHB; vkGetMemoryFdKHR hands out a stand-in. */
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
struct mv_dmabuf_reg { struct mv_dmabuf_reg *next; VkDeviceMemory memory; int socket; };
/* Drop the Xwayland registrations of one memory (or all, at device teardown). */
static void mv_dmabuf_unregister(struct mv_device *d,VkDeviceMemory memory,int all) {
    struct mv_dmabuf_reg *drop=0,**link;
    while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
    for(link=&d->dmabuf_regs;*link;) {
        struct mv_dmabuf_reg *r=*link;
        if(all||r->memory==memory){*link=r->next;r->next=drop;drop=r;}else link=&r->next;
    }
    __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);
    while(drop){struct mv_dmabuf_reg *next=drop->next;mv_x11_ahb_unregister(drop->socket);mv_free(drop);drop=next;}
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
    struct mv_dmabuf_reg *reg=mv_alloc(sizeof(*reg));if(!reg)return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct AHardwareBuffer *ahb=0;struct mv_ahb_desc desc={0};int pair[2]={-1,-1},standin=-1,sent=-1;
    VkMemoryGetAndroidHardwareBufferInfoANDROID get={.sType=VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,.memory=info->memory};
    struct mv_scope s;if(!mv_enter(&s)){mv_free(reg);return VK_ERROR_INITIALIZATION_FAILED;}
    VkResult r=d->ahb_export(d->base.android,&get,&ahb);
    if(!r&&ahb) {
        mv_ahb_describe(ahb,&desc);
        int sp=mv_socketpair(pair);
        if(sp)mv_trace_result("maton-vulkan: dma-buf export socketpair failed",sp,0);
        else sent=mv_ahb_send_socket(ahb,pair[1]);
        mv_ahb_release(ahb); /* the transport carries its own reference */
    }
    switch_back(&s);
    if(r||!ahb){mv_trace_result("maton-vulkan: dma-buf export: memory has no AHB",r,0);mv_free(reg);return r?r:VK_ERROR_INVALID_EXTERNAL_HANDLE;}
    if(sent){
        if(pair[0]>=0)mv_trace_result("maton-vulkan: dma-buf export AHB send failed",sent,0);
        if(pair[0]>=0){mv_close(pair[0]);mv_close(pair[1]);}
        mv_free(reg);return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    mv_close(pair[1]);
    /* 4 bytes per pixel: the WSI/X formats this path serves are 32 bpp. */
    standin=mv_memfd("maton-vk-dmabuf",(uint64_t)desc.stride*4*desc.height);
    if(standin<0){mv_trace_result("maton-vulkan: dma-buf stand-in memfd failed",standin,0);mv_close(pair[0]);mv_free(reg);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    reg->socket=mv_x11_ahb_register(standin,pair[0]);
    mv_close(pair[0]);
    if(reg->socket<0) {
        mv_trace_result("maton-vulkan: dma-buf stand-in register failed (stage*1000+errno)",mv_x11_last_error,0);
        mv_close(standin);mv_free(reg);return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    reg->memory=info->memory;
    while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
    reg->next=d->dmabuf_regs;d->dmabuf_regs=reg;
    __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);
    mv_trace_result("maton-vulkan: dma-buf exported stand-in fd, AHB format",standin,desc.format);
    *fd=standin;return VK_SUCCESS;
}
