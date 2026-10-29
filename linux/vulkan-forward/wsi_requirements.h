#ifndef MATON_WSI_REQUIREMENTS_H
#define MATON_WSI_REQUIREMENTS_H
/* These names are enabled on the resident device by device.inc. Advertise a
 * Linux swapchain only when that allocation/presentation backend can work. */
static const char *const mv_wsi_wayland_requirements[]={
    "VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign",
    "VK_EXT_image_drm_format_modifier", "VK_KHR_image_format_list", "VK_KHR_external_semaphore_fd"
};
static const char *const mv_wsi_x11_requirements[]={
    "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
    "VK_EXT_image_drm_format_modifier", "VK_KHR_image_format_list", "VK_KHR_external_semaphore_fd"
};
static int mv_wsi_has_requirements(const char *const *names,size_t size,const VkExtensionProperties *all,uint32_t count){
    for(size_t i=0;i<size;i++){
        uint32_t j=0;while(j<count&&strcmp(names[i],all[j].extensionName))j++;
        if(j==count)return 0;
    }
    return 1;
}
#endif
