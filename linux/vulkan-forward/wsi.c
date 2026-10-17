#include "wsi.h"
#include "runtime.h"
#ifndef MV_WSI_PROTOTYPE
#define MV_WSI_PROTOTYPE 0
#endif
/* Public NDK AHB format numbers used by maton-ahb-v1; avoid a bionic
 * header dependency in the freestanding Linux ICD. Usage comes from the
 * negotiated transport/allocator, and must cover the driver's full answer. */
VkResult mv_wsi_query_limits(VkPhysicalDevice physical,PFN_vkGetPhysicalDeviceImageFormatProperties2 query,
    uint32_t count,const uint32_t *transport_formats,uint64_t transport_usage,struct mv_wsi_limits *out) {
    memset(out,0,sizeof(*out));
    if(!MV_WSI_PROTOTYPE)return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(!query || (count && !transport_formats))return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_wsi_limits result={0};
    const uint32_t wire[]={1,5};
    const VkFormat formats[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM};
    for(uint32_t i=0;i<2;i++){
        int negotiated=0;
        for(uint32_t j=0;j<count;j++)if(transport_formats[j]==wire[i])negotiated=1;
        if(!negotiated)continue;
        VkPhysicalDeviceExternalImageFormatInfo external={
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
        VkPhysicalDeviceImageFormatInfo2 info={
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,.pNext=&external,
            .format=formats[i],.type=VK_IMAGE_TYPE_2D,.tiling=VK_IMAGE_TILING_OPTIMAL,
            .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT};
        VkAndroidHardwareBufferUsageANDROID usage={.sType=VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID};
        VkExternalImageFormatProperties ep={.sType=VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,.pNext=&usage};
        VkImageFormatProperties2 p={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,.pNext=&ep};
        VkResult r=query(physical,&info,&p);
        if(r==VK_ERROR_FORMAT_NOT_SUPPORTED)continue;
        if(r)return r;
        VkExternalMemoryProperties *em=&ep.externalMemoryProperties;
        if(!(em->externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
           !(em->compatibleHandleTypes&external.handleType) ||
           (usage.androidHardwareBufferUsage&transport_usage)!=usage.androidHardwareBufferUsage ||
           !(p.imageFormatProperties.sampleCounts&VK_SAMPLE_COUNT_1_BIT) ||
           !p.imageFormatProperties.maxArrayLayers || !p.imageFormatProperties.maxMipLevels ||
           !p.imageFormatProperties.maxExtent.width || !p.imageFormatProperties.maxExtent.height ||
           !p.imageFormatProperties.maxExtent.depth)continue;
        /* wlroots dimensions are signed; this is a representation constraint. */
        uint32_t w=p.imageFormatProperties.maxExtent.width,h=p.imageFormatProperties.maxExtent.height;
        if(w>INT32_MAX)w=INT32_MAX;
        if(h>INT32_MAX)h=INT32_MAX;
        if(!result.format_count){result.max_extent=(VkExtent2D){w,h};}
        else {
            if(w<result.max_extent.width)result.max_extent.width=w;
            if(h<result.max_extent.height)result.max_extent.height=h;
        }
        result.formats[result.format_count++]=(VkSurfaceFormatKHR){formats[i],VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    }
    if(!result.format_count)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *out=result;return VK_SUCCESS;
}
void mv_wsi_surface_init(struct mv_wsi_surface *s,uint64_t token,VkExtent2D e) {
    *s=(struct mv_wsi_surface){.token=token,.generation=1,.extent=e,.alive=token!=0};
}
void mv_wsi_surface_resize(struct mv_wsi_surface *s,VkExtent2D e) { s->extent=e;s->generation++; }
VkBool32 mv_wsi_support(const struct mv_wsi_surface *s,VkQueueFlags flags,int ahb) {
    return MV_WSI_PROTOTYPE && s->alive && s->limits.format_count && ahb && (flags&VK_QUEUE_GRAPHICS_BIT)!=0;
}
VkResult mv_wsi_capabilities(const struct mv_wsi_surface *s,VkSurfaceCapabilitiesKHR *c) {
    if(!MV_WSI_PROTOTYPE || !s->alive)return VK_ERROR_SURFACE_LOST_KHR;
    if(!s->limits.format_count)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(!s->extent.width || !s->extent.height ||
       s->extent.width>s->limits.max_extent.width || s->extent.height>s->limits.max_extent.height)
        return VK_ERROR_OUT_OF_DATE_KHR;
    *c=(VkSurfaceCapabilitiesKHR){.minImageCount=2,.maxImageCount=0,
        .currentExtent=s->extent,.minImageExtent={1,1},.maxImageExtent=s->limits.max_extent,
        .maxImageArrayLayers=1,.supportedTransforms=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .currentTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .supportedCompositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .supportedUsageFlags=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    return VK_SUCCESS;
}
VkResult mv_wsi_formats(const struct mv_wsi_surface *s,uint32_t *n,VkSurfaceFormatKHR *f) {
    if(!MV_WSI_PROTOTYPE){*n=0;return VK_ERROR_EXTENSION_NOT_PRESENT;}
    if(!s->alive){*n=0;return VK_ERROR_SURFACE_LOST_KHR;}
    if(!s->limits.format_count){*n=0;return VK_ERROR_FORMAT_NOT_SUPPORTED;}
    if(!f){*n=s->limits.format_count;return VK_SUCCESS;}
    uint32_t len=*n<s->limits.format_count?*n:s->limits.format_count;
    for(uint32_t i=0;i<len;i++)f[i]=s->limits.formats[i];
    *n=len;return len<s->limits.format_count?VK_INCOMPLETE:VK_SUCCESS;
}
VkResult mv_wsi_modes(uint32_t *n,VkPresentModeKHR *m) {
    if(!MV_WSI_PROTOTYPE){*n=0;return VK_ERROR_EXTENSION_NOT_PRESENT;}
    if(!m){*n=1;return VK_SUCCESS;}if(!*n)return VK_INCOMPLETE;
    *n=1;*m=VK_PRESENT_MODE_FIFO_KHR;return VK_SUCCESS;
}
static VkResult status(struct mv_wsi_swapchain *c) {
    if(!c->surface->alive)return VK_ERROR_SURFACE_LOST_KHR;
    if(c->generation!=c->surface->generation)return VK_ERROR_OUT_OF_DATE_KHR;
    return VK_SUCCESS;
}
static void cleanup(struct mv_wsi_swapchain *c) {
    for(uint32_t i=0;i<c->count;i++){
        struct mv_wsi_image *b=&c->images[i];
        if(b->image || b->memory)c->ops.destroy(c->ops.context,b->image,b->memory);
        if(b->ahb)c->ops.release(c->ops.context,b->ahb,b->token);
    }
    mv_free(c->images);mv_free(c);
}
VkResult mv_wsi_create(struct mv_wsi_surface *s,const VkSwapchainCreateInfoKHR *info,const struct mv_wsi_ops *ops,struct mv_wsi_swapchain **out) {
    *out=0;
    VkSurfaceCapabilitiesKHR caps;VkResult r=mv_wsi_capabilities(s,&caps);if(r)return r;
    int format_ok=0;
    for(uint32_t i=0;i<s->limits.format_count;i++)
        if(info->imageFormat==s->limits.formats[i].format && info->imageColorSpace==s->limits.formats[i].colorSpace)format_ok=1;
    if(info->sType!=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR || info->pNext || info->flags ||
       info->minImageCount<caps.minImageCount || info->imageArrayLayers!=1 ||
       !format_ok ||
       info->imageUsage!=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT || info->imageSharingMode!=VK_SHARING_MODE_EXCLUSIVE ||
       info->preTransform!=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR || info->compositeAlpha!=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ||
       info->presentMode!=VK_PRESENT_MODE_FIFO_KHR)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(!s->extent.width || !s->extent.height || s->extent.width>caps.maxImageExtent.width || s->extent.height>caps.maxImageExtent.height ||
       info->imageExtent.width!=s->extent.width || info->imageExtent.height!=s->extent.height)return VK_ERROR_OUT_OF_DATE_KHR;
    if(info->oldSwapchain)return VK_ERROR_FEATURE_NOT_PRESENT;
    if(!ops || !ops->allocate || !ops->release || !ops->import || !ops->destroy || !ops->present)return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_wsi_swapchain *c=mv_alloc(sizeof(*c));if(!c)return VK_ERROR_OUT_OF_HOST_MEMORY;
    *c=(struct mv_wsi_swapchain){.surface=s,.ops=*ops,.generation=s->generation,.count=info->minImageCount};
    if((size_t)c->count>SIZE_MAX/sizeof(*c->images)){mv_free(c);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    c->images=mv_alloc((size_t)c->count*sizeof(*c->images));if(!c->images){mv_free(c);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    memset(c->images,0,(size_t)c->count*sizeof(*c->images));
    for(uint32_t i=0;i<c->count;i++){
        struct mv_wsi_image *b=&c->images[i];
        r=ops->allocate(ops->context,s->token,s->extent,info->imageFormat,&b->ahb,&b->token);
        if(!r)r=ops->import(ops->context,b->ahb,s->extent,info->imageFormat,&b->image,&b->memory);
        if(r){cleanup(c);return r;}
    }
    *out=c;return VK_SUCCESS;
}
VkResult mv_wsi_images(struct mv_wsi_swapchain *c,uint32_t *n,VkImage *images) {
    if(!images){*n=c->count;return VK_SUCCESS;}
    uint32_t len=*n<c->count?*n:c->count;
    for(uint32_t i=0;i<len;i++)images[i]=c->images[i].image;
    *n=len;return len<c->count?VK_INCOMPLETE:VK_SUCCESS;
}
VkResult mv_wsi_acquire(struct mv_wsi_swapchain *c,uint32_t *index) {
    VkResult r=status(c);if(r)return r;
    for(uint32_t i=0;i<c->count;i++)if(!c->images[i].state){c->images[i].state=1;*index=i;return VK_SUCCESS;}
    return VK_NOT_READY;
}
VkResult mv_wsi_present(struct mv_wsi_swapchain *c,uint32_t index) {
    VkResult r=status(c);if(r)return r;
    if(index>=c->count || c->images[index].state!=1)return VK_ERROR_INITIALIZATION_FAILED;
    struct mv_wsi_image *b=&c->images[index];
    if(c->serial==UINT64_MAX)return VK_ERROR_OUT_OF_DATE_KHR;
    uint64_t serial=++c->serial;
    r=c->ops.present(c->ops.context,c->surface->token,b->token,serial);
    if(!r){b->serial=serial;b->state=2;}return r;
}
VkResult mv_wsi_release(struct mv_wsi_swapchain *c,uint64_t token,uint64_t serial) {
    for(uint32_t i=0;i<c->count;i++){
        struct mv_wsi_image *b=&c->images[i];
        if(b->token==token && b->serial==serial && b->state==2){b->state=0;return VK_SUCCESS;}
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}
VkResult mv_wsi_destroy(struct mv_wsi_swapchain *c) {
    for(uint32_t i=0;i<c->count;i++)if(c->images[i].state==2)return VK_NOT_READY;
    cleanup(c);return VK_SUCCESS;
}
VkResult mv_wsi_import_ahb(struct mv_wsi_driver *d,struct AHardwareBuffer *ahb,VkExtent2D e,VkFormat format,VkImage *image,VkDeviceMemory *memory) {
    *image=VK_NULL_HANDLE;*memory=VK_NULL_HANDLE;
    if(!MV_WSI_PROTOTYPE)return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(!d || !ahb || !d->properties || !d->create || !d->allocate || !d->bind || !d->destroy || !d->free)return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkAndroidHardwareBufferFormatPropertiesANDROID f={.sType=VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID p={.sType=VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,.pNext=&f};
    VkResult r=d->properties(d->device,ahb,&p);if(r)return r;
    if(f.format!=format || !(f.formatFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) || !p.memoryTypeBits || !p.allocationSize)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkExternalMemoryImageCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&external,.imageType=VK_IMAGE_TYPE_2D,
        .format=format,.extent={e.width,e.height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    r=d->create(d->device,&ci,0,image);if(r)return r;
    VkMemoryDedicatedAllocateInfo dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=*image};
    VkImportAndroidHardwareBufferInfoANDROID import={.sType=VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,.pNext=&dedicated,.buffer=ahb};
    uint32_t type=0;while(!(p.memoryTypeBits&(1u<<type)))type++;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&import,.allocationSize=p.allocationSize,.memoryTypeIndex=type};
    r=d->allocate(d->device,&ai,0,memory);
    if(!r)r=d->bind(d->device,*image,*memory,0);
    if(r){d->destroy(d->device,*image,0);if(*memory)d->free(d->device,*memory,0);*image=VK_NULL_HANDLE;*memory=VK_NULL_HANDLE;}
    return r;
}
