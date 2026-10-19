#define VK_USE_PLATFORM_ANDROID_KHR
#define _GNU_SOURCE
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define LOAD(dst, name) do { *(void **)(&(dst)) = gipa(instance, name); if (!(dst)) { printf("missing %s\n", name); goto done; } } while (0)

static int has_extension(const char *name, VkExtensionProperties *props, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (!strcmp(name, props[i].extensionName)) return 1;
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    void *lib = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) { puts("missing vkGetInstanceProcAddr"); return 1; }
    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties enum_ie = (PFN_vkEnumerateInstanceExtensionProperties)gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    uint32_t ien = 0; VkExtensionProperties *iep = NULL;
    if (enum_ie) { enum_ie(NULL, &ien, NULL); iep = calloc(ien, sizeof(*iep)); enum_ie(NULL, &ien, iep); }
    const char *iext[] = { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME };
    uint32_t ic = 0; for (unsigned j=0;j<2;j++) if (has_extension(iext[j],iep,ien)) ic++;
    const char *ienabled[2]; unsigned k=0; for(unsigned j=0;j<2;j++) if(has_extension(iext[j],iep,ien)) ienabled[k++]=iext[j];
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="ahb-native-probe",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,.enabledExtensionCount=ic,.ppEnabledExtensionNames=ienabled};
    VkInstance instance=VK_NULL_HANDLE; VkResult r=create_instance(&ici,NULL,&instance); printf("vkCreateInstance=%d enabled_instance_extensions=%u\n",r,ic); if(r) return 2;
    PFN_vkEnumeratePhysicalDevices enum_pd; LOAD(enum_pd,"vkEnumeratePhysicalDevices");
    uint32_t n=0; r=enum_pd(instance,&n,NULL); VkPhysicalDevice *pds=calloc(n,sizeof(*pds)); if(!n||r){printf("enumerate physical devices=%d count=%u\n",r,n);goto done;}
    enum_pd(instance,&n,pds); VkPhysicalDevice pd=pds[0];
    PFN_vkGetPhysicalDeviceProperties getprop; LOAD(getprop,"vkGetPhysicalDeviceProperties"); VkPhysicalDeviceProperties props; getprop(pd,&props); printf("physical_device=%s api=%u.%u.%u\n",props.deviceName,VK_VERSION_MAJOR(props.apiVersion),VK_VERSION_MINOR(props.apiVersion),VK_VERSION_PATCH(props.apiVersion));
    PFN_vkEnumerateDeviceExtensionProperties enum_de; LOAD(enum_de,"vkEnumerateDeviceExtensionProperties"); uint32_t den=0; enum_de(pd,NULL,&den,NULL); VkExtensionProperties *dep=calloc(den,sizeof(*dep)); enum_de(pd,NULL,&den,dep);
    const char *dext[]={VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME};
    const char *enabled[4]; uint32_t dc=0; for(unsigned j=0;j<4;j++){int yes=has_extension(dext[j],dep,den);printf("device_extension %s=%s\n",dext[j],yes?"yes":"no");if(yes)enabled[dc++]=dext[j];}
    if(!has_extension(dext[0],dep,den)){puts("AHB extension unavailable");goto done;}
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getq; LOAD(getq,"vkGetPhysicalDeviceQueueFamilyProperties");uint32_t qn=0;getq(pd,&qn,NULL);VkQueueFamilyProperties *qps=calloc(qn,sizeof(*qps));getq(pd,&qn,qps);uint32_t qi=0;while(qi<qn&&!(qps[qi].queueFlags&VK_QUEUE_GRAPHICS_BIT))qi++;if(qi==qn){puts("no graphics queue");goto done;}
    float priority=1.f;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=qi,.queueCount=1,.pQueuePriorities=&priority};VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.enabledExtensionCount=dc,.ppEnabledExtensionNames=enabled};PFN_vkCreateDevice create_device;LOAD(create_device,"vkCreateDevice");VkDevice device=VK_NULL_HANDLE;r=create_device(pd,&dci,NULL,&device);printf("vkCreateDevice=%d enabled_device_extensions=%u\n",r,dc);if(r)goto done;
    PFN_vkGetDeviceProcAddr gdpa=(PFN_vkGetDeviceProcAddr)gipa(instance,"vkGetDeviceProcAddr");
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID ahb_query=(PFN_vkGetAndroidHardwareBufferPropertiesANDROID)gdpa(device,"vkGetAndroidHardwareBufferPropertiesANDROID");
    PFN_vkGetPhysicalDeviceFormatProperties getfmt; LOAD(getfmt,"vkGetPhysicalDeviceFormatProperties"); VkFormatProperties format_props; getfmt(pd,VK_FORMAT_R8G8B8A8_UNORM,&format_props); printf("RGBA8 formatProperties linear=0x%x optimal=0x%x buffer=0x%x\n",format_props.linearTilingFeatures,format_props.optimalTilingFeatures,format_props.bufferFeatures);
    if(!ahb_query){puts("missing AHB query entry point");goto done;}
    const uint64_t usages[]={AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT|AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE|AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY,AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT|AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT};
    const char *usage_env=getenv("AHB_USAGE_INDEX"); unsigned first=usage_env?(unsigned)atoi(usage_env):0, last=usage_env?first+1:3; if(last>3){puts("AHB_USAGE_INDEX must be 0..2");goto done;}
    for(unsigned u=first;u<last;u++){
        AHardwareBuffer_Desc desc={.width=500,.height=500,.layers=1,.format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,.usage=usages[u]};AHardwareBuffer *buffer=NULL;int ar=AHardwareBuffer_allocate(&desc,&buffer);printf("AHardwareBuffer_allocate usage=0x%llx result=%d ptr=%p\n",(unsigned long long)usages[u],ar,(void*)buffer);if(ar||!buffer)continue;
        VkAndroidHardwareBufferFormatPropertiesANDROID fp={.sType=VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};VkAndroidHardwareBufferPropertiesANDROID ap={.sType=VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,.pNext=&fp};r=ahb_query(device,buffer,&ap);printf("vkGetAndroidHardwareBufferPropertiesANDROID result=%d allocationSize=%llu memoryTypeBits=0x%x format=%d externalFormat=%llu formatFeatures=0x%llx\n",r,(unsigned long long)ap.allocationSize,ap.memoryTypeBits,fp.format,(unsigned long long)fp.externalFormat,(unsigned long long)fp.formatFeatures);
        PFN_vkCreateImage ci_img=(PFN_vkCreateImage)gdpa(device,"vkCreateImage");PFN_vkAllocateMemory alloc=(PFN_vkAllocateMemory)gdpa(device,"vkAllocateMemory");PFN_vkBindImageMemory bind=(PFN_vkBindImageMemory)gdpa(device,"vkBindImageMemory");PFN_vkDestroyImage destroy=(PFN_vkDestroyImage)gdpa(device,"vkDestroyImage");PFN_vkFreeMemory free_mem=(PFN_vkFreeMemory)gdpa(device,"vkFreeMemory");
        if(r==VK_SUCCESS&&ap.memoryTypeBits&&ci_img&&alloc&&bind&&destroy&&free_mem){VkExternalMemoryImageCreateInfo ex={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};VkImageCreateInfo imgci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&ex,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.extent={500,500,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};VkImage image=VK_NULL_HANDLE;VkResult ir=ci_img(device,&imgci,NULL,&image);printf("vkCreateImage import-capable result=%d\n",ir);if(ir==VK_SUCCESS){VkImportAndroidHardwareBufferInfoANDROID imp={.sType=VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,.buffer=buffer};VkMemoryDedicatedAllocateInfo ded={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.pNext=&imp,.image=image};VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&ded,.allocationSize=ap.allocationSize,.memoryTypeIndex=__builtin_ctz(ap.memoryTypeBits)};VkDeviceMemory mem=VK_NULL_HANDLE;VkResult mr=alloc(device,&mai,NULL,&mem);printf("vkAllocateMemory AHB import result=%d memoryTypeIndex=%u\n",mr,mai.memoryTypeIndex);if(mr==VK_SUCCESS){printf("vkBindImageMemory result=%d\n",bind(device,image,mem,0));free_mem(device,mem,NULL);}destroy(device,image,NULL);}}
        AHardwareBuffer_release(buffer);
    }
done:
    return 0;
}
