/* Host-only native driver stand-in. Real bionic, resident adoption, bootstrap
 * discovery and the freestanding ICD remain production code. */
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <vulkan/vulkan.h>
__attribute__((constructor)) static void native_environment(void) {
    unsetenv("LD_PRELOAD");unsetenv("LD_LIBRARY_PATH");
}
EGLDisplay eglGetDisplay(EGLNativeDisplayType display) {
    (void)display;return (EGLDisplay)(uintptr_t)0x1234;
}
/* Preload may discover a display, but must not start driver workers before
 * a Linux zygote's single-thread check. The Vulkan payload never calls EGL. */
EGLBoolean eglInitialize(EGLDisplay display,EGLint *major,EGLint *minor) {
    (void)display;(void)major;(void)minor;_exit(98);
}
void *dlopen(const char *path,int flags) {
    void *(*next)(const char *,int)=dlsym(RTLD_NEXT,"dlopen");
    if(path && (!strcmp(path,"/system/lib/libvulkan.so") || !strcmp(path,"/system/lib64/libvulkan.so")))path=getenv("MATON_TEST_GPU_PROVIDER");
    if(path && (!strcmp(path,"/system/lib/libandroid.so") || !strcmp(path,"/system/lib64/libandroid.so")))return NULL;
    return next(path,flags);
}
static VkResult create(const VkInstanceCreateInfo *info,const VkAllocationCallbacks *a,VkInstance *out) {
    (void)info;(void)a;*out=(VkInstance)(uintptr_t)0x12345678;return VK_SUCCESS;
}
static void destroy(VkInstance instance,const VkAllocationCallbacks *a) {(void)instance;(void)a;}
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance,const char *name) {
    (void)instance;
    if(!strcmp(name,"vkCreateInstance"))return (PFN_vkVoidFunction)create;
    if(!strcmp(name,"vkDestroyInstance"))return (PFN_vkVoidFunction)destroy;
    return NULL;
}
