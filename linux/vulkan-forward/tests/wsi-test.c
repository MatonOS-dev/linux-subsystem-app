#define _POSIX_C_SOURCE 200809L
#include "wsi.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
/* Mock endpoint protocol: native host-only fixed record, no pointer payloads.
 * The child holds presented tokens until an explicit RELEASE request.
 * This is NOT the future Wayland extension nor an AHB transport emulator. */
struct message { uint64_t op,surface,buffer,serial; };
static unsigned query_calls,query_fault;
static VkResult format_query(VkPhysicalDevice physical,const VkPhysicalDeviceImageFormatInfo2 *info,VkImageFormatProperties2 *out) {
    assert(physical==(VkPhysicalDevice)(uintptr_t)7);
    assert(info->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2);
    const VkPhysicalDeviceExternalImageFormatInfo *external=info->pNext;
    assert(external->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO &&
        external->handleType==VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID && !external->pNext);
    assert(info->type==VK_IMAGE_TYPE_2D && info->tiling==VK_IMAGE_TILING_OPTIMAL && !info->flags);
    assert(info->usage==(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT));
    assert(info->format==VK_FORMAT_R8G8B8A8_UNORM || info->format==VK_FORMAT_B8G8R8A8_UNORM);
    VkExternalImageFormatProperties *ep=out->pNext;
    VkAndroidHardwareBufferUsageANDROID *usage=ep->pNext;
    assert(out->sType==VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 &&
        ep->sType==VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES &&
        usage->sType==VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID && !usage->pNext);
    query_calls++;
    if(query_fault==1)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(query_fault==2 && info->format==VK_FORMAT_B8G8R8A8_UNORM)return VK_ERROR_OUT_OF_HOST_MEMORY;
    ep->externalMemoryProperties.externalMemoryFeatures=query_fault==3?0:VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    ep->externalMemoryProperties.compatibleHandleTypes=query_fault==4?0:external->handleType;
    usage->androidHardwareBufferUsage=query_fault==5?0x10000:0x300;
    out->imageFormatProperties=(VkImageFormatProperties){
        .maxExtent={info->format==VK_FORMAT_R8G8B8A8_UNORM?16384:8192,12288,1},
        .maxMipLevels=1,.maxArrayLayers=query_fault==6?0:1,.sampleCounts=VK_SAMPLE_COUNT_1_BIT};
    if(query_fault==7)out->imageFormatProperties.maxExtent=(VkExtent3D){UINT32_MAX,UINT32_MAX,1};
    return VK_SUCCESS;
}
static void query_tests(struct mv_wsi_surface *s) {
    uint32_t wire[]={1,5,1,999};struct mv_wsi_limits limits;
    VkPhysicalDevice physical=(VkPhysicalDevice)(uintptr_t)7;
    assert(!mv_wsi_query_limits(physical,format_query,4,wire,0xb00,&limits));
    assert(query_calls==2 && limits.format_count==2 && limits.max_extent.width==8192 && limits.max_extent.height==12288);
    s->limits=limits;
    for(query_fault=1;query_fault<=6;query_fault++){
        limits=s->limits;
        assert(mv_wsi_query_limits(physical,format_query,4,wire,0xb00,&limits)==
            (query_fault==2?VK_ERROR_OUT_OF_HOST_MEMORY:VK_ERROR_FORMAT_NOT_SUPPORTED));
        assert(!limits.format_count && !limits.max_extent.width);
    }
    query_fault=7;assert(!mv_wsi_query_limits(physical,format_query,4,wire,0xb00,&limits));
    assert(limits.max_extent.width==INT32_MAX && limits.max_extent.height==INT32_MAX);
    query_fault=0;query_calls=0;
    assert(!mv_wsi_query_limits(physical,format_query,1,wire,0xb00,&limits));
    assert(query_calls==1 && limits.format_count==1 && limits.max_extent.width==16384);
    s->limits=limits;
    assert(mv_wsi_query_limits(physical,format_query,1,0,0xb00,&limits)==VK_ERROR_INITIALIZATION_FAILED);
    assert(!limits.format_count);
    assert(mv_wsi_query_limits(physical,format_query,0,0,0xb00,&limits)==VK_ERROR_FORMAT_NOT_SUPPORTED);
    assert(!limits.format_count);
}
static int endpoint;
static unsigned allocations,imports,destroys,releases,fail_import,fail_bind;
static unsigned image_creates,memory_allocs,image_destroys,memory_frees;
static struct AHardwareBuffer *last_ahb;
static VkImage last_image;
static struct message exchange(struct message m) {
    struct message reply;
    assert(send(endpoint,&m,sizeof(m),0)==sizeof(m));
    assert(recv(endpoint,&reply,sizeof(reply),0)==sizeof(reply));return reply;
}
static VkResult allocate(void *ctx,uint64_t surface,VkExtent2D e,VkFormat f,struct AHardwareBuffer **b,uint64_t *token) {
    (void)ctx;assert(surface==42 && e.width==640 && e.height==480 && f==VK_FORMAT_R8G8B8A8_UNORM);
    *token=++allocations;*b=(void *)(uintptr_t)(0x1000+*token);return VK_SUCCESS;
}
static void release(void *ctx,struct AHardwareBuffer *b,uint64_t token) { (void)ctx;assert((uintptr_t)b==0x1000+token);releases++; }
static void destroy(void *ctx,VkImage image,VkDeviceMemory memory) { (void)ctx;assert(image && memory);destroys++; }
static VkResult import(void *ctx,struct AHardwareBuffer *b,VkExtent2D e,VkFormat f,VkImage *image,VkDeviceMemory *memory) {
    (void)ctx;(void)e;(void)f;imports++;if(imports==fail_import)return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *image=(VkImage)(uintptr_t)b;*memory=(VkDeviceMemory)((uintptr_t)b+0x1000);return VK_SUCCESS;
}
static VkResult present(void *ctx,uint64_t surface,uint64_t buffer,uint64_t serial) {
    (void)ctx;struct message r=exchange((struct message){1,surface,buffer,serial});assert(r.op==1);return VK_SUCCESS;
}
static VkResult properties(VkDevice d,const struct AHardwareBuffer *b,VkAndroidHardwareBufferPropertiesANDROID *p) {
    (void)d;assert(b==last_ahb);p->allocationSize=8192;p->memoryTypeBits=8;
    VkAndroidHardwareBufferFormatPropertiesANDROID *f=p->pNext;
    assert(f->sType==VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID);
    f->format=VK_FORMAT_R8G8B8A8_UNORM;f->formatFeatures=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;return VK_SUCCESS;
}
static VkResult create_image(VkDevice d,const VkImageCreateInfo *ci,const VkAllocationCallbacks *a,VkImage *i) {
    (void)d;assert(!a && ci->extent.width==640 && ci->extent.height==480 && ci->mipLevels==1 && ci->arrayLayers==1);
    assert(ci->tiling==VK_IMAGE_TILING_OPTIMAL && ci->initialLayout==VK_IMAGE_LAYOUT_UNDEFINED);
    const VkExternalMemoryImageCreateInfo *ext=ci->pNext;
    assert(ext->sType==VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO && ext->handleTypes==VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
    *i=(VkImage)(uintptr_t)99;last_image=*i;image_creates++;return VK_SUCCESS;
}
static VkResult allocate_memory(VkDevice d,const VkMemoryAllocateInfo *ai,const VkAllocationCallbacks *a,VkDeviceMemory *m) {
    (void)d;assert(!a && ai->allocationSize==8192 && ai->memoryTypeIndex==3);
    const VkImportAndroidHardwareBufferInfoANDROID *imp=ai->pNext;
    assert(imp->sType==VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID && imp->buffer==last_ahb);
    const VkMemoryDedicatedAllocateInfo *ded=imp->pNext;
    assert(ded->sType==VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO && ded->image==last_image && !ded->buffer && !ded->pNext);
    *m=(VkDeviceMemory)(uintptr_t)100;memory_allocs++;return VK_SUCCESS;
}
static VkResult bind_memory(VkDevice d,VkImage i,VkDeviceMemory m,VkDeviceSize offset) {
    (void)d;assert(i==last_image && m==(VkDeviceMemory)(uintptr_t)100 && !offset);
    return fail_bind?VK_ERROR_DEVICE_LOST:VK_SUCCESS;
}
static void destroy_image(VkDevice d,VkImage i,const VkAllocationCallbacks *a) { (void)d;assert(!a && i==last_image);image_destroys++; }
static void free_memory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks *a) { (void)d;assert(!a && m==(VkDeviceMemory)(uintptr_t)100);memory_frees++; }
static void run_endpoint(int fd) {
    struct message pending[4]={{0}},m;
    while(recv(fd,&m,sizeof(m),0)==sizeof(m)){
        if(m.op==3)_exit(0);
        if(m.op==1){assert(m.surface==42 && m.buffer>0);unsigned i=0;while(i<4 && pending[i].op)i++;assert(i<4);pending[i]=m;}
        else {assert(m.op==2);unsigned i=0;while(i<4 && pending[i].buffer!=m.buffer)i++;assert(i<4 && pending[i].op);m=pending[i];m.op=2;pending[i].op=0;}
        assert(send(fd,&m,sizeof(m),0)==sizeof(m));
    }
    _exit(1);
}
int main(void) {
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,sockets));pid_t child=fork();assert(child>=0);
    if(!child){close(sockets[0]);run_endpoint(sockets[1]);}
    close(sockets[1]);endpoint=sockets[0];
    struct mv_wsi_surface s;mv_wsi_surface_init(&s,42,(VkExtent2D){640,480});
    assert(!mv_wsi_support(&s,VK_QUEUE_GRAPHICS_BIT,1));
    query_tests(&s);
    assert(mv_wsi_support(&s,VK_QUEUE_GRAPHICS_BIT,1));assert(!mv_wsi_support(&s,VK_QUEUE_COMPUTE_BIT,1));assert(!mv_wsi_support(&s,VK_QUEUE_GRAPHICS_BIT,0));
    VkSurfaceCapabilitiesKHR caps;assert(!mv_wsi_capabilities(&s,&caps));assert(caps.currentExtent.width==640 && caps.minImageCount==2 && !caps.maxImageCount && caps.maxImageExtent.width==16384);
    uint32_t n=0;assert(!mv_wsi_formats(&s,&n,0) && n==1);VkSurfaceFormatKHR f; n=0;assert(mv_wsi_formats(&s,&n,&f)==VK_INCOMPLETE);n=1;assert(!mv_wsi_formats(&s,&n,&f));
    VkPresentModeKHR mode;n=1;assert(!mv_wsi_modes(&n,&mode) && mode==VK_PRESENT_MODE_FIFO_KHR);
    struct mv_wsi_ops ops={0,allocate,release,import,destroy,present};
    VkSwapchainCreateInfoKHR ci={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.minImageCount=2,
        .imageFormat=f.format,.imageColorSpace=f.colorSpace,.imageExtent={640,480},.imageArrayLayers=1,
        .imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,.presentMode=VK_PRESENT_MODE_FIFO_KHR};
    struct mv_wsi_swapchain *c=0;
    ci.presentMode=VK_PRESENT_MODE_MAILBOX_KHR;assert(mv_wsi_create(&s,&ci,&ops,&c)==VK_ERROR_FORMAT_NOT_SUPPORTED && !c);ci.presentMode=mode;
    ci.minImageCount=5;assert(!mv_wsi_create(&s,&ci,&ops,&c));assert(c->count==5);assert(!mv_wsi_destroy(c));
    allocations=imports=destroys=releases=0;ci.minImageCount=2;
    fail_import=2;assert(mv_wsi_create(&s,&ci,&ops,&c)==VK_ERROR_OUT_OF_DEVICE_MEMORY && !c);assert(releases==2 && destroys==1);
    fail_import=0;assert(!mv_wsi_create(&s,&ci,&ops,&c));VkImage images[2];n=1;assert(mv_wsi_images(c,&n,images)==VK_INCOMPLETE && n==1);n=2;assert(!mv_wsi_images(c,&n,images));assert(images[0]!=images[1]);
    uint32_t a,b;assert(!mv_wsi_acquire(c,&a));assert(!mv_wsi_present(c,a));assert(mv_wsi_present(c,a)==VK_ERROR_INITIALIZATION_FAILED);
    assert(!mv_wsi_acquire(c,&b) && a!=b);assert(!mv_wsi_present(c,b));assert(mv_wsi_acquire(c,&a)==VK_NOT_READY);
    assert(mv_wsi_destroy(c)==VK_NOT_READY);
    struct message r=exchange((struct message){2,42,c->images[0].token,0});assert(mv_wsi_release(c,r.buffer,r.serial+1)==VK_ERROR_INITIALIZATION_FAILED);assert(!mv_wsi_release(c,r.buffer,r.serial));assert(mv_wsi_release(c,r.buffer,r.serial)==VK_ERROR_INITIALIZATION_FAILED);
    assert(!mv_wsi_acquire(c,&a) && a==0);assert(!mv_wsi_present(c,a));assert(mv_wsi_release(c,r.buffer,r.serial)==VK_ERROR_INITIALIZATION_FAILED);
    mv_wsi_surface_resize(&s,(VkExtent2D){800,600});assert(mv_wsi_acquire(c,&a)==VK_ERROR_OUT_OF_DATE_KHR);
    for(unsigned i=0;i<2;i++){r=exchange((struct message){2,42,c->images[i].token,0});assert(!mv_wsi_release(c,r.buffer,r.serial));}
    s.alive=0;assert(mv_wsi_acquire(c,&a)==VK_ERROR_SURFACE_LOST_KHR);assert(!mv_wsi_destroy(c));assert(releases==4 && destroys==3);
    struct mv_wsi_driver d={.properties=properties,.create=create_image,.allocate=allocate_memory,.bind=bind_memory,.destroy=destroy_image,.free=free_memory};
    last_ahb=(void *)(uintptr_t)123;VkImage image;VkDeviceMemory memory;
    assert(!mv_wsi_import_ahb(&d,last_ahb,(VkExtent2D){640,480},f.format,&image,&memory));destroy_image(0,image,0);free_memory(0,memory,0);
    fail_bind=1;assert(mv_wsi_import_ahb(&d,last_ahb,(VkExtent2D){640,480},f.format,&image,&memory)==VK_ERROR_DEVICE_LOST);assert(!image && !memory);
    assert(image_creates==2 && memory_allocs==2 && image_destroys==2 && memory_frees==2);
    struct message stop={.op=3};assert(send(endpoint,&stop,sizeof(stop),0)==sizeof(stop));close(endpoint);int result;assert(waitpid(child,&result,0)==child && WIFEXITED(result) && !WEXITSTATUS(result));
    puts("PASS WSI: resident external-image queries, negotiated formats, uncapped image count, surface queries, enumeration, import rollback, backpressure, cross-process present/release, stale release, resize/loss, dedicated AHB import chains and bind rollback");
}
