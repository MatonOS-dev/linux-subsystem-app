/* Native resident-driver extension inventory; no Linux ICD/loader involved. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { VkResult r=(x);if(r){fprintf(stderr,"%s: %d\n",#x,r);return 1;} } while(0)
int main(void) {
 VkApplicationInfo a={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
 VkInstanceCreateInfo c={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&a};
 VkInstance i;CHECK(vkCreateInstance(&c,NULL,&i));uint32_t n=1;VkPhysicalDevice p;CHECK(vkEnumeratePhysicalDevices(i,&n,&p));if(!n)return 1;
 n=0;CHECK(vkEnumerateDeviceExtensionProperties(p,NULL,&n,NULL));VkExtensionProperties *e=calloc(n,sizeof(*e));if(!e)return 1;
 CHECK(vkEnumerateDeviceExtensionProperties(p,NULL,&n,e));for(uint32_t k=0;k<n;k++)printf("%s %u\n",e[k].extensionName,e[k].specVersion);
 free(e);vkDestroyInstance(i,NULL);return 0;
}
