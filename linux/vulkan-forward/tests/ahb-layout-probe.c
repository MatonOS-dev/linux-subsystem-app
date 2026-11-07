/* ahb-layout-probe: does a RADV write into a gralloc AHB read back correctly
 * through radeonsi (EGL)? Compares the two ways RADV can bind such a buffer:
 *   ahb - VK_ANDROID_external_memory_android_hardware_buffer import
 *         (what the MatonOS Vulkan forwarder WSI does)
 *   anb - VkNativeBufferANDROID (what the stock Android swapchain does)
 * Usage: ahb-layout-probe ahb|anb [width height]  (exit 0 = layout matches) */
#define VK_USE_PLATFORM_ANDROID_KHR
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

/* From vk_android_native_buffer.h (not in the NDK). */
#define MV_STYPE_NATIVE_BUFFER_ANDROID ((VkStructureType)1000010000)
typedef struct { uint64_t consumer, producer; } MvUsage2;
typedef struct {
    VkStructureType sType; const void *pNext; const void *handle;
    int stride, format, usage; MvUsage2 usage2; uint64_t usage3; AHardwareBuffer *ahb;
} MvNativeBuffer;
extern const void *AHardwareBuffer_getNativeHandle(const AHardwareBuffer *);

#define CHECK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d\n", #x, r_); return 2; } } while (0)

static uint32_t pattern(uint32_t x, uint32_t y) { /* bytes in memory: B G R A */
    return (x & 0xff) | ((y & 0xff) << 8) | ((((x >> 8) & 0xf) | (((y >> 8) & 0xf) << 4)) << 16) | 0xff000000u;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2 || (strcmp(argv[1], "ahb") && strcmp(argv[1], "anb"))) { puts("usage: ahb-layout-probe ahb|anb [w h]"); return 2; }
    int anb = !strcmp(argv[1], "anb");
    uint32_t w = argc > 3 ? (uint32_t)atoi(argv[2]) : 1280, h = argc > 3 ? (uint32_t)atoi(argv[3]) : 800;

    AHardwareBuffer_Desc desc = {.width = w, .height = h, .layers = 1, .format = 5 /* HAL BGRA_8888 */,
        .usage = 0xb00 /* GPU sampled | GPU color output | composer overlay, as the forwarder */};
    AHardwareBuffer *ahb = NULL;
    if (AHardwareBuffer_allocate(&desc, &ahb) || !ahb) { puts("FAIL AHardwareBuffer_allocate"); return 2; }
    AHardwareBuffer_describe(ahb, &desc);
    printf("ahb %ux%u stride=%u mode=%s\n", desc.width, desc.height, desc.stride, argv[1]);

    void *lib = dlopen("libvulkan.so", RTLD_NOW);
    PFN_vkGetInstanceProcAddr gipa = lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
    if (!gipa) { puts("FAIL libvulkan"); return 2; }
#define IF(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    VkInstance inst = VK_NULL_HANDLE;
    IF(vkCreateInstance);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    CHECK(vkCreateInstance(&(VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app}, NULL, &inst));
    IF(vkEnumeratePhysicalDevices); IF(vkCreateDevice); IF(vkGetDeviceProcAddr); IF(vkGetPhysicalDeviceMemoryProperties);
    IF(vkGetPhysicalDeviceProperties);
    uint32_t n = 1; VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) { puts("FAIL no device"); return 2; }
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    printf("vulkan device: %s\n", props.deviceName);
    const char *dext[] = {"VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign", "VK_KHR_external_memory"};
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci, .enabledExtensionCount = 3, .ppEnabledExtensionNames = dext}, NULL, &dev));
#define DF(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    DF(vkGetDeviceQueue); DF(vkCreateImage); DF(vkAllocateMemory); DF(vkBindImageMemory); DF(vkCreateBuffer);
    DF(vkGetBufferMemoryRequirements); DF(vkBindBufferMemory); DF(vkMapMemory); DF(vkCreateCommandPool);
    DF(vkAllocateCommandBuffers); DF(vkBeginCommandBuffer); DF(vkEndCommandBuffer); DF(vkCmdPipelineBarrier);
    DF(vkCmdCopyBufferToImage); DF(vkQueueSubmit); DF(vkQueueWaitIdle); DF(vkGetAndroidHardwareBufferPropertiesANDROID);
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    VkImage img;
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (anb) {
        MvNativeBuffer nb = {.sType = MV_STYPE_NATIVE_BUFFER_ANDROID, .handle = AHardwareBuffer_getNativeHandle(ahb),
            .stride = (int)desc.stride, .format = 5, .usage = 0xb00, .usage3 = 0xb00, .ahb = ahb};
        ici.pNext = &nb;
        CHECK(vkCreateImage(dev, &ici, NULL, &img)); /* driver imports and binds the memory itself */
    } else {
        VkExternalMemoryImageCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
        ici.pNext = &ext;
        CHECK(vkCreateImage(dev, &ici, NULL, &img));
        VkAndroidHardwareBufferPropertiesANDROID hp = {.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
        CHECK(vkGetAndroidHardwareBufferPropertiesANDROID(dev, ahb, &hp));
        uint32_t t = 0; while (t < 32 && !(hp.memoryTypeBits & (1u << t))) t++;
        VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img};
        VkImportAndroidHardwareBufferInfoANDROID imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .pNext = &ded, .buffer = ahb};
        VkDeviceMemory m;
        CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
            .allocationSize = hp.allocationSize, .memoryTypeIndex = t}, NULL, &m));
        CHECK(vkBindImageMemory(dev, img, m, 0));
    }

    /* Staging buffer with a per-pixel unique pattern. */
    VkDeviceSize size = (VkDeviceSize)w * h * 4;
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT}, NULL, &buf));
    VkMemoryRequirements br; vkGetBufferMemoryRequirements(dev, buf, &br);
    uint32_t bt = 0;
    while (bt < mp.memoryTypeCount && !((br.memoryTypeBits & (1u << bt)) &&
           (mp.memoryTypes[bt].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) bt++;
    VkDeviceMemory bm;
    CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = br.size, .memoryTypeIndex = bt}, NULL, &bm));
    CHECK(vkBindBufferMemory(dev, buf, bm, 0));
    uint32_t *px; CHECK(vkMapMemory(dev, bm, 0, size, 0, (void **)&px));
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) px[y * w + x] = pattern(x, y);

    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}, NULL, &pool));
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1}, &cb));
    CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}));
    VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b1 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = rng};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(cb, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
    /* Hand the image to the "foreign" consumer (radeonsi), as a present would. */
    VkImageMemoryBarrier b2 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = 0, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT, .image = img, .subresourceRange = rng};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
    CHECK(vkEndCommandBuffer(cb));
    CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb}, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(q));

    /* Read back through radeonsi, the way Xwayland / SurfaceFlinger import it. */
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(d, NULL, NULL)) { puts("FAIL eglInitialize"); return 2; }
    printf("egl: %s\n", eglQueryString(d, EGL_VENDOR));
    EGLConfig cfg; EGLint nc = 0;
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    eglChooseConfig(d, ca, &cfg, 1, &nc);
    EGLint cta[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, cta);
    EGLint pa[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, cfg, pa);
    if (!ctx || !eglMakeCurrent(d, s, s, ctx)) { puts("FAIL egl context"); return 2; }
    printf("gl: %s\n", (const char *)glGetString(GL_RENDERER));
    EGLImageKHR ei = eglCreateImageKHR(d, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, eglGetNativeClientBufferANDROID(ahb), NULL);
    if (ei == EGL_NO_IMAGE_KHR) { printf("FAIL eglCreateImageKHR 0x%x\n", eglGetError()); return 2; }
    GLuint tex, fbo; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, ei);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { puts("FAIL fbo incomplete"); return 2; }
    uint8_t *rb = malloc(size);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, rb);
    if (glGetError()) { puts("FAIL glReadPixels"); return 2; }

    uint64_t bad = 0; long first = -1;
    for (uint32_t i = 0; i < w * h; i++) {
        uint32_t e = pattern(i % w, i / w);
        uint8_t *p = rb + (size_t)i * 4; /* R G B A */
        uint32_t got = p[2] | (p[1] << 8) | (p[0] << 16) | ((uint32_t)p[3] << 24);
        if (got != e) { bad++; if (first < 0) first = i; }
    }
    if (bad) {
        uint8_t *p = rb + (size_t)first * 4;
        uint32_t gx = p[2] | ((p[0] & 0xf) << 8), gy = p[1] | ((p[0] >> 4) << 8);
        printf("RESULT %s: MISMATCH %llu/%u pixels; first at (%ld,%ld) holds the pixel written to (%u,%u)\n", argv[1],
               (unsigned long long)bad, w * h, first % w, first / w, gx, gy);
        return 1;
    }
    printf("RESULT %s: MATCH all %u pixels\n", argv[1], w * h);
    return 0;
}
