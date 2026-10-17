/* D2 feasibility gate. Run as a native bionic process, before enabling WSI.
 * Does not create a Surface, install an APK, or touch application data. */
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

static int has(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i=0;i<n;i++) if (!strcmp(e[i].extensionName,name)) return 1;
    return 0;
}
int main(void) {
    VkInstance instance=VK_NULL_HANDLE;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkResult r=vkCreateInstance(&ci,NULL,&instance);
    if(r){printf("create instance: %d\n",r);return 1;}
    uint32_t n=0; r=vkEnumeratePhysicalDevices(instance,&n,NULL);
    if(r || !n){vkDestroyInstance(instance,NULL);return 1;}
    VkPhysicalDevice *devices=calloc(n,sizeof(*devices));
    if(!devices){vkDestroyInstance(instance,NULL);return 1;}
    r=vkEnumeratePhysicalDevices(instance,&n,devices);
    int blocked=0;
    for(uint32_t i=0;!r && i<n;i++) {
        VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(devices[i],&p);
        printf("device: %s api=%u.%u.%u\n",p.deviceName,VK_API_VERSION_MAJOR(p.apiVersion),VK_API_VERSION_MINOR(p.apiVersion),VK_API_VERSION_PATCH(p.apiVersion));
        uint32_t count=0;vkEnumerateDeviceExtensionProperties(devices[i],NULL,&count,NULL);
        VkExtensionProperties *e=calloc(count,sizeof(*e));
        if(!e){blocked=1;break;}
        VkResult er=vkEnumerateDeviceExtensionProperties(devices[i],NULL,&count,e);
        const char *names[]={VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME};
        for(unsigned j=0;j<sizeof(names)/sizeof(names[0]);j++)printf("%s: %s\n",names[j],!er && has(e,count,names[j])?"yes":"NO");
        int ahb=!er && has(e,count,names[0]);free(e);
        if(!ahb){puts("BLOCKED: resident driver cannot import AHardwareBuffer images");blocked=1;continue;}
        VkFormat formats[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM};
        int supported=0;
        for(unsigned j=0;j<2;j++){
            VkPhysicalDeviceExternalImageFormatInfo external={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
            VkPhysicalDeviceImageFormatInfo2 query={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,.pNext=&external,.format=formats[j],.type=VK_IMAGE_TYPE_2D,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT};
            VkAndroidHardwareBufferUsageANDROID usage={.sType=VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID};
            VkExternalImageFormatProperties ep={.sType=VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,.pNext=&usage};
            VkImageFormatProperties2 result={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,.pNext=&ep};
            VkResult fr=vkGetPhysicalDeviceImageFormatProperties2(devices[i],&query,&result);
            printf("format=%d query=%d external_features=0x%x ahb_usage=0x%llx\n",formats[j],fr,ep.externalMemoryProperties.externalMemoryFeatures,(unsigned long long)usage.androidHardwareBufferUsage);
            if(!fr && (ep.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) supported=1;
        }
        if(!supported){puts("BLOCKED: neither RGBA8 nor BGRA8 AHB color attachment is importable");blocked=1;}
    }
    /* Allocation alone is insufficient; report it separately from import. */
    AHardwareBuffer_Desc desc={.width=64,.height=64,.layers=1,.format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,.usage=AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT|AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE|AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY};
    AHardwareBuffer *buffer=NULL;int ar=AHardwareBuffer_allocate(&desc,&buffer);
    printf("AHB allocation (RGBA8, color output + sampled + composer): %d\n",ar);
    if(buffer && n && !r) {
        uint32_t qc=0;vkGetPhysicalDeviceQueueFamilyProperties(devices[0],&qc,NULL);
        VkQueueFamilyProperties *qp=calloc(qc,sizeof(*qp));
        if(!qp){AHardwareBuffer_release(buffer);free(devices);vkDestroyInstance(instance,NULL);return 1;}
        vkGetPhysicalDeviceQueueFamilyProperties(devices[0],&qc,qp);
        uint32_t q=0;while(q<qc && !(qp[q].queueFlags&VK_QUEUE_GRAPHICS_BIT))q++;
        free(qp);
        float priority=1;
        VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=q,.queueCount=1,.pQueuePriorities=&priority};
        const char *extension=VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
        VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.enabledExtensionCount=1,.ppEnabledExtensionNames=&extension};
        VkDevice device=VK_NULL_HANDLE;
        VkResult dr=q<qc?vkCreateDevice(devices[0],&di,NULL,&device):VK_ERROR_INITIALIZATION_FAILED;
        printf("device for AHB import: %d\n",dr);
        if(!dr){
            PFN_vkGetAndroidHardwareBufferPropertiesANDROID properties=(PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(device,"vkGetAndroidHardwareBufferPropertiesANDROID");
            VkAndroidHardwareBufferFormatPropertiesANDROID format={.sType=VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
            VkAndroidHardwareBufferPropertiesANDROID props={.sType=VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,.pNext=&format};
            dr=properties?properties(device,buffer,&props):VK_ERROR_EXTENSION_NOT_PRESENT;
            printf("actual AHB properties: %d format=%d features=0x%x memoryTypes=0x%x allocation=%llu\n",dr,format.format,format.formatFeatures,props.memoryTypeBits,(unsigned long long)props.allocationSize);
            if(!dr && format.format==VK_FORMAT_R8G8B8A8_UNORM && props.memoryTypeBits){
                VkExternalMemoryImageCreateInfo ex={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
                VkImageCreateInfo image_ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&ex,.imageType=VK_IMAGE_TYPE_2D,.format=format.format,.extent={64,64,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
                VkImage image=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
                dr=vkCreateImage(device,&image_ci,NULL,&image);
                if(!dr){
                    VkMemoryDedicatedAllocateInfo dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=image};
                    VkImportAndroidHardwareBufferInfoANDROID import={.sType=VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,.pNext=&dedicated,.buffer=buffer};
                    uint32_t mt=0;while(!(props.memoryTypeBits&(1u<<mt)))mt++;
                    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&import,.allocationSize=props.allocationSize,.memoryTypeIndex=mt};
                    dr=vkAllocateMemory(device,&ai,NULL,&memory);
                    if(!dr)dr=vkBindImageMemory(device,image,memory,0);
                }
                printf("actual AHB create + dedicated import + bind: %d\n",dr);
                if(image)vkDestroyImage(device,image,NULL);
                if(memory)vkFreeMemory(device,memory,NULL);
            }else dr=VK_ERROR_FORMAT_NOT_SUPPORTED;
            vkDestroyDevice(device,NULL);
        }
        if(dr)blocked=1;
    }
    if(ar)blocked=1;
    if(buffer)AHardwareBuffer_release(buffer);
    void *native=dlopen("libandroid.so",RTLD_NOW|RTLD_LOCAL);
    printf("ASurfaceTransaction_setBufferWithRelease: %s\n",native && dlsym(native,"ASurfaceTransaction_setBufferWithRelease")?"yes":"NO");
    if(native)dlclose(native);
    free(devices);vkDestroyInstance(instance,NULL);
    return r || blocked?2:0;
}
