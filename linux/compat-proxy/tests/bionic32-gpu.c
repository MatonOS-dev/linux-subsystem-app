#include <dlfcn.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
#define CHECK(x) do {if(!(x)){printf("FAIL bionic32 GPU line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc,char **argv) {
    CHECK(argc==2);
    void *lib=dlopen(argv[1],2) /* glibc RTLD_NOW, independent of NDK constants */;if(!lib)puts(dlerror());CHECK(lib);
    PFN_vkGetInstanceProcAddr gipa=(void *)dlsym(lib,"vk_icdGetInstanceProcAddr");CHECK(gipa);
    PFN_vkCreateInstance create=(void *)gipa(NULL,"vkCreateInstance");CHECK(create);
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance instance=NULL;CHECK(create(&info,NULL,&instance)==VK_SUCCESS && instance);
    PFN_vkDestroyInstance destroy=(void *)gipa(instance,"vkDestroyInstance");CHECK(destroy);
    destroy(instance,NULL);
    puts("PASS kernel ELF64 -> bionic32 resident -> real bootstrap/adoption -> maton ICD vkCreateInstance (test driver)");return 0;
}
