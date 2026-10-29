/* Device adapters execute allocations/callbacks exclusively on application TLS. */
static void *mv_dnew(struct mv_device *d,size_t n) {
    void *p=d->custom?d->allocator.pfnAllocation(d->allocator.pUserData,n,_Alignof(max_align_t),VK_SYSTEM_ALLOCATION_SCOPE_DEVICE):mv_alloc(n);
    if(p)memset(p,0,n);return p;
}
static void mv_dfree(struct mv_device *d,void *p) {
    if(d->custom)d->allocator.pfnFree(d->allocator.pUserData,p);else mv_free(p);
}
static struct mv_handle *mv_dwrap(struct mv_device *d,void *raw,VkCommandPool pool) {
    while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
    struct mv_handle *h=d->children;
    while(h && h->android!=raw)h=h->next;
    if(!h){h=mv_dnew(d,sizeof(*h));if(h){set_loader_magic_value(h);h->android=raw;h->device=d;h->instance=d->base.instance;h->pool=pool;h->next=d->children;d->children=h;}}
    __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);return h;
}
static void mv_chain_free(VkBaseOutStructure *p) {
    while(p){VkBaseOutStructure *next=p->pNext;
        if(p->sType==VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO)mv_free((void *)((VkDeviceGroupDeviceCreateInfo *)p)->pPhysicalDevices);
        mv_free(p);p=next;}
}
static VkResult mv_chain_copy(const void *source,VkBaseOutStructure **out) {
    *out=0;VkBaseOutStructure **tail=out;
    for(const VkBaseInStructure *p=source;p;p=p->pNext){
        /* Khronos-loader private bookkeeping contains Linux callbacks. The
         * Android loader supplies its own nodes; never forward ours. */
        if(p->sType==VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)continue;
        size_t n=mv_chain_size(p->sType);if(!n){mv_chain_free(*out);*out=0;return VK_ERROR_FEATURE_NOT_PRESENT;}
        VkBaseOutStructure *copy=mv_alloc(n);if(!copy){mv_chain_free(*out);*out=0;return VK_ERROR_OUT_OF_HOST_MEMORY;}
        memcpy(copy,p,n);copy->pNext=0;*tail=copy;tail=&copy->pNext;
        if(p->sType==VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO){
            const VkDeviceGroupDeviceCreateInfo *group=(const void *)p;VkDeviceGroupDeviceCreateInfo *g=(void *)copy;
            g->pPhysicalDevices=0;
            VkPhysicalDevice *raw=mv_alloc((size_t)g->physicalDeviceCount*sizeof(*raw));
            if(!raw){mv_chain_free(*out);*out=0;return VK_ERROR_OUT_OF_HOST_MEMORY;}
            g->pPhysicalDevices=raw;
            for(uint32_t j=0;j<g->physicalDeviceCount;j++)raw[j]=((struct mv_handle *)group->pPhysicalDevices[j])->android;
        }
    }
    return VK_SUCCESS;
}
static VkResult mv_vkCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo *info,const VkAllocationCallbacks *a,VkDevice *out) {
    *out=0;struct mv_handle *h=(void *)physical;
    uint32_t forwarded_extensions=0;int swapchain_enabled=0;
    for(uint32_t j=0;j<info->enabledExtensionCount;j++) {
        const char *name=info->ppEnabledExtensionNames[j];
        if(!strcmp(name,VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            if(!h->instance->wsi_wayland_enabled&&!h->instance->wsi_xcb_enabled&&!h->instance->wsi_xlib_enabled)return VK_ERROR_EXTENSION_NOT_PRESENT;
            swapchain_enabled=1;
        } else {
            if(!mv_extension_allowed(name))return VK_ERROR_EXTENSION_NOT_PRESENT;
            forwarded_extensions++;
        }
    }
    if(a && (!a->pfnAllocation || !a->pfnFree || !a->pfnReallocation))return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_device seed={0};if(a){seed.custom=1;seed.allocator=*a;}
    struct mv_device *d=mv_dnew(&seed,sizeof(*d));if(!d)return VK_ERROR_OUT_OF_HOST_MEMORY;
    d->custom=seed.custom;d->allocator=seed.allocator;d->base.instance=h->instance;d->base.device=d;d->wsi_swapchain_enabled=swapchain_enabled;
    for(uint32_t j=0;j<info->enabledExtensionCount;j++)if(!strcmp(info->ppEnabledExtensionNames[j],VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME))d->wsi_sync_fd_enabled=1;
    VkDeviceCreateInfo copy=*info;
    const char **extension_names=0;
    if(swapchain_enabled) {
        static const char *const wayland_internal[]={
            "VK_ANDROID_external_memory_android_hardware_buffer",
            "VK_EXT_queue_family_foreign", "VK_EXT_image_drm_format_modifier",
            "VK_KHR_image_format_list", "VK_KHR_external_semaphore_fd"
        };
        static const char *const x11_internal[]={
            "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
            "VK_EXT_image_drm_format_modifier", "VK_KHR_image_format_list", "VK_KHR_external_semaphore_fd"
        };
        /* WSI choice belongs to the surface, not the application. A mixed
         * X11/Wayland instance needs the union of both allocation backends. */
        const char *internal[10];uint32_t internal_count=0;
        if(h->instance->wsi_wayland_enabled)for(uint32_t j=0;j<5;j++)internal[internal_count++]=wayland_internal[j];
        if(h->instance->wsi_xcb_enabled||h->instance->wsi_xlib_enabled)for(uint32_t j=0;j<5;j++)internal[internal_count++]=x11_internal[j];
        extension_names=mv_alloc((forwarded_extensions+internal_count)*sizeof(*extension_names));
        if(!extension_names){mv_dfree(d,d);return VK_ERROR_OUT_OF_HOST_MEMORY;}
        uint32_t k=0;
        for(uint32_t j=0;j<info->enabledExtensionCount;j++)
            if(strcmp(info->ppEnabledExtensionNames[j],VK_KHR_SWAPCHAIN_EXTENSION_NAME))extension_names[k++]=info->ppEnabledExtensionNames[j];
        for(uint32_t j=0;j<internal_count;j++) {
            uint32_t n=0;while(n<k && strcmp(extension_names[n],internal[j]))n++;
            if(n==k)extension_names[k++]=internal[j];
        }
        copy.enabledExtensionCount=k;copy.ppEnabledExtensionNames=extension_names;
        d->wsi_sync_fd_enabled=1;
    }
    VkBaseOutStructure *chain=0;
    VkResult cr=mv_chain_copy(info->pNext,&chain);
    if(cr!=VK_SUCCESS){mv_free(extension_names);mv_dfree(d,d);return cr;}
    copy.pNext=chain;
    /* Validate each copied node independently; group handles now belong to
     * Android, while all other nodes must contain no callbacks/foreign handles. */
    for(VkBaseOutStructure *p=chain;p;p=p->pNext){
        VkBaseOutStructure *next=p->pNext;p->pNext=0;
        int valid=p->sType==VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO || mv_chain_ok(p);
        p->pNext=next;
        if(!valid){mv_chain_free(chain);mv_free(extension_names);mv_dfree(d,d);return VK_ERROR_FEATURE_NOT_PRESENT;}
    }
    for(uint32_t j=0;j<info->queueCreateInfoCount;j++)if(!mv_valid_VkDeviceQueueCreateInfo(&info->pQueueCreateInfos[j])){mv_chain_free(chain);mv_free(extension_names);mv_dfree(d,d);return VK_ERROR_FEATURE_NOT_PRESENT;}
    struct mv_scope s;if(!mv_enter(&s)){mv_chain_free(chain);mv_free(extension_names);mv_dfree(d,d);return VK_ERROR_INITIALIZATION_FAILED;}
    PFN_vkCreateDevice f=h->instance->create_device;
    mv_trace_result("maton-vulkan: enter create device",0,0);
    VkDevice raw=0;VkResult r=f((VkPhysicalDevice)h->android,&copy,0,&raw);
    mv_trace_result("maton-vulkan: leave create device",r,0);
    if(r==VK_SUCCESS){d->base.android=raw;d->gdpa=h->instance->gdpa;mv_load_device(d);}
    switch_back(&s);mv_chain_free(chain);mv_free(extension_names);
    if(r!=VK_SUCCESS){mv_dfree(d,d);return r;}
    set_loader_magic_value(d);*out=(VkDevice)d;return VK_SUCCESS;
}
static void mv_vkDestroyDevice(VkDevice device,const VkAllocationCallbacks *a) {
    (void)a;if(!device)return;struct mv_device *d=(void *)device;struct mv_scope s;if(!mv_enter(&s))return;
    d->dispatch.vkDestroyDevice(d->base.android,0);switch_back(&s);
    struct mv_handle *h=d->children;while(h){struct mv_handle *next=h->next;mv_dfree(d,h);h=next;}mv_dfree(d,d);
}
static void mv_vkGetDeviceQueue2(VkDevice device,const VkDeviceQueueInfo2 *info,VkQueue *out) {
    if(!mv_valid_VkDeviceQueueInfo2(info)){*out=0;return;}
    struct mv_device *d=(void *)device;*out=0;VkQueue raw=0;struct mv_scope s;if(!mv_enter(&s))return;
    d->dispatch.vkGetDeviceQueue2(d->base.android,info,&raw);switch_back(&s);
    if(raw)*out=(VkQueue)mv_dwrap(d,raw,0);
}
static void mv_vkGetDeviceQueue(VkDevice device,uint32_t family,uint32_t index,VkQueue *out) {
    struct mv_device *d=(void *)device;*out=0;VkQueue raw=0;struct mv_scope s;if(!mv_enter(&s))return;
    d->dispatch.vkGetDeviceQueue(d->base.android,family,index,&raw);switch_back(&s);
    if(raw)*out=(VkQueue)mv_dwrap(d,raw,0);
}
static VkResult mv_vkAllocateCommandBuffers(VkDevice device,const VkCommandBufferAllocateInfo *info,VkCommandBuffer *out) {
    if(!mv_valid_VkCommandBufferAllocateInfo(info))return VK_ERROR_FEATURE_NOT_PRESENT;
    struct mv_device *d=(void *)device;
    /* Preallocate all wrappers so failure cannot leak Android command buffers. */
    uint32_t n=info->commandBufferCount;struct mv_handle **wr=mv_alloc((size_t)n*sizeof(*wr));if(!wr)return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t j=0;for(;j<n;j++)if(!(wr[j]=mv_dnew(d,sizeof(**wr))))break;
    if(j<n){while(j)mv_dfree(d,wr[--j]);mv_free(wr);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    struct mv_scope s;VkResult r=VK_ERROR_INITIALIZATION_FAILED;
    if(mv_enter(&s)){r=d->dispatch.vkAllocateCommandBuffers(d->base.android,info,out);switch_back(&s);}
    if(r==VK_SUCCESS){
        while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
        for(j=0;j<n;j++){struct mv_handle *h=wr[j];set_loader_magic_value(h);h->android=out[j];h->device=d;h->instance=d->base.instance;h->pool=info->commandPool;h->next=d->children;d->children=h;out[j]=(VkCommandBuffer)h;}
        __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);
    }else for(j=0;j<n;j++)mv_dfree(d,wr[j]);
    mv_free(wr);return r;
}
static void mv_vkFreeCommandBuffers(VkDevice device,VkCommandPool pool,uint32_t n,const VkCommandBuffer *buffers) {
    struct mv_device *d=(void *)device;VkCommandBuffer *raw=mv_alloc((size_t)n*sizeof(*raw));if(!raw)return;
    for(uint32_t j=0;j<n;j++)raw[j]=buffers[j]?(VkCommandBuffer)((struct mv_handle *)buffers[j])->android:0;
    struct mv_scope s;if(!mv_enter(&s)){mv_free(raw);return;}
    d->dispatch.vkFreeCommandBuffers(d->base.android,pool,n,raw);switch_back(&s);mv_free(raw);
    while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
    for(uint32_t j=0;j<n;j++){struct mv_handle **p=&d->children;while(*p && *p!=(void *)buffers[j])p=&(*p)->next;if(*p){struct mv_handle *h=*p;*p=h->next;mv_dfree(d,h);}}
    __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);
}
static void mv_vkDestroyCommandPool(VkDevice device,VkCommandPool pool,const VkAllocationCallbacks *a) {
    (void)a;struct mv_device *d=(void *)device;struct mv_scope s;if(!mv_enter(&s))return;
    d->dispatch.vkDestroyCommandPool(d->base.android,pool,0);switch_back(&s);
    while(__atomic_exchange_n(&d->lock,1,__ATOMIC_ACQUIRE)){}
    struct mv_handle **p=&d->children;
    while(*p){struct mv_handle *h=*p;if(h->pool==pool && pool){*p=h->next;mv_dfree(d,h);}else p=&h->next;}
    __atomic_store_n(&d->lock,0,__ATOMIC_RELEASE);
}
static VkResult mv_vkQueueSubmit(VkQueue queue,uint32_t n,const VkSubmitInfo *submits,VkFence fence) {
    for(uint32_t j=0;j<n;j++)if(!mv_chain_ok(submits[j].pNext))return VK_ERROR_FEATURE_NOT_PRESENT;
    struct mv_handle *h=(void *)queue;VkSubmitInfo *copy=mv_alloc((size_t)n*sizeof(*copy));if(!copy)return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t j=0;VkResult r=VK_ERROR_OUT_OF_HOST_MEMORY;
    for(;j<n;j++){copy[j]=submits[j];size_t count=submits[j].commandBufferCount;VkCommandBuffer *cmd=mv_alloc(count*sizeof(*cmd));if(!cmd)break;for(size_t k=0;k<count;k++)cmd[k]=((struct mv_handle *)submits[j].pCommandBuffers[k])->android;copy[j].pCommandBuffers=cmd;}
    if(j==n){struct mv_scope s;if(mv_enter(&s)){r=h->device->dispatch.vkQueueSubmit(h->android,n,copy,fence);switch_back(&s);}else r=VK_ERROR_INITIALIZATION_FAILED;}
    while(j)mv_free((void *)copy[--j].pCommandBuffers);mv_free(copy);return r;
}
static VkResult mv_vkQueueSubmit2(VkQueue queue,uint32_t n,const VkSubmitInfo2 *submits,VkFence fence) {
    for(uint32_t j=0;j<n;j++){
        if(!mv_chain_ok(submits[j].pNext))return VK_ERROR_FEATURE_NOT_PRESENT;
        for(uint32_t k=0;k<submits[j].commandBufferInfoCount;k++)if(!mv_chain_ok(submits[j].pCommandBufferInfos[k].pNext))return VK_ERROR_FEATURE_NOT_PRESENT;
        for(uint32_t k=0;k<submits[j].waitSemaphoreInfoCount;k++)if(!mv_valid_VkSemaphoreSubmitInfo(&submits[j].pWaitSemaphoreInfos[k]))return VK_ERROR_FEATURE_NOT_PRESENT;
        for(uint32_t k=0;k<submits[j].signalSemaphoreInfoCount;k++)if(!mv_valid_VkSemaphoreSubmitInfo(&submits[j].pSignalSemaphoreInfos[k]))return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    struct mv_handle *h=(void *)queue;VkSubmitInfo2 *copy=mv_alloc((size_t)n*sizeof(*copy));if(!copy)return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t j=0;VkResult r=VK_ERROR_OUT_OF_HOST_MEMORY;
    for(;j<n;j++){copy[j]=submits[j];size_t count=submits[j].commandBufferInfoCount;VkCommandBufferSubmitInfo *cmd=mv_alloc(count*sizeof(*cmd));if(!cmd)break;for(size_t k=0;k<count;k++){cmd[k]=submits[j].pCommandBufferInfos[k];cmd[k].commandBuffer=((struct mv_handle *)cmd[k].commandBuffer)->android;}copy[j].pCommandBufferInfos=cmd;}
    if(j==n){struct mv_scope s;if(mv_enter(&s)){r=h->device->dispatch.vkQueueSubmit2(h->android,n,copy,fence);switch_back(&s);}else r=VK_ERROR_INITIALIZATION_FAILED;}
    while(j)mv_free((void *)copy[--j].pCommandBufferInfos);mv_free(copy);return r;
}
static void mv_vkCmdExecuteCommands(VkCommandBuffer buffer,uint32_t n,const VkCommandBuffer *commands) {
    struct mv_handle *h=(void *)buffer;VkCommandBuffer *raw=mv_alloc((size_t)n*sizeof(*raw));if(!raw)return;
    for(uint32_t j=0;j<n;j++)raw[j]=((struct mv_handle *)commands[j])->android;
    struct mv_scope s;if(mv_enter(&s)){h->device->dispatch.vkCmdExecuteCommands(h->android,n,raw);switch_back(&s);}mv_free(raw);
}
static PFN_vkVoidFunction mv_lookup(const char *name);
static PFN_vkVoidFunction mv_vkGetDeviceProcAddr(VkDevice device,const char *name) {
    if(!device || !name)return 0;struct mv_device *d=(void *)device;
    PFN_vkVoidFunction local=mv_wsi_device_lookup(device,name);if(local)return local;
    local=mv_lookup(name);if(!local)return 0;
    struct mv_scope s;if(!mv_enter(&s))return 0;PFN_vkVoidFunction raw=d->gdpa(d->base.android,name);switch_back(&s);return raw?local:0;
}
