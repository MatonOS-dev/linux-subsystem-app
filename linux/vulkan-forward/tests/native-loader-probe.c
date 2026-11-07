#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
int main(void){void *lib=dlopen("/system/lib64/libvulkan.so",RTLD_NOW);if(!lib){puts(dlerror());return 1;}PFN_vkGetInstanceProcAddr g=(void *)dlsym(lib,"vkGetInstanceProcAddr");PFN_vkCreateInstance c=(void *)g(0,"vkCreateInstance");VkApplicationInfo a={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&a};VkInstance i=0;VkResult r=c(&ci,0,&i);printf("native vkCreateInstance=%d\n",r);if(r)return 1;PFN_vkEnumeratePhysicalDevices e=(void *)g(i,"vkEnumeratePhysicalDevices");uint32_t n=0;r=e(i,&n,0);printf("native vkEnumeratePhysicalDevices=%d count=%u\n",r,n);return r!=0;}
