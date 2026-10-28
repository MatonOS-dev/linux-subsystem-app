/* Host-only native driver stand-in. Real bionic, resident adoption, bootstrap
 * discovery and the freestanding ICD remain production code. */
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <vulkan/vulkan.h>
__attribute__((constructor)) static void native_environment(void) {
    unsetenv("LD_PRELOAD");unsetenv("LD_LIBRARY_PATH");
}
EGLDisplay eglGetDisplay(EGLNativeDisplayType display) {(void)display;return EGL_NO_DISPLAY;}
void *dlopen(const char *path,int flags) {
    void *(*next)(const char *,int)=dlsym(RTLD_NEXT,"dlopen");
    if(path && !strcmp(path,"/system/lib/libvulkan.so"))path=getenv("MATON_TEST_GPU_PROVIDER");
    if(path && !strcmp(path,"/system/lib/libandroid.so"))return NULL;
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
