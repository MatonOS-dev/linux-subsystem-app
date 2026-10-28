#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <vulkan/vulkan.h>
#define CHECK(x) do {if(!(x)){printf("FAIL bionic32 GPU line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int query(const char *icd) {
    void *lib=dlopen(icd,2) /* glibc RTLD_NOW, independent of NDK constants */;if(!lib)puts(dlerror());CHECK(lib);
    PFN_vkGetInstanceProcAddr gipa=(void *)dlsym(lib,"vk_icdGetInstanceProcAddr");CHECK(gipa);
    PFN_vkCreateInstance create=(void *)gipa(NULL,"vkCreateInstance");CHECK(create);
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance instance=NULL;CHECK(create(&info,NULL,&instance)==VK_SUCCESS && instance);
    PFN_vkDestroyInstance destroy=(void *)gipa(instance,"vkDestroyInstance");CHECK(destroy);
    destroy(instance,NULL);
    return 0;
}
int main(int argc,char **argv) {
    CHECK(argc==2 || argc==3);
    if(argc==2) {
        pid_t child=fork();CHECK(child>=0);
        if(!child)_exit(query(argv[1]));
        int status;CHECK(waitpid(child,&status,0)==child && status==0);
        CHECK(!query(argv[1]));
        char *next[]={argv[0],argv[1],"resident-exec",NULL};
        execv(argv[0],next);CHECK(0);
    }
    CHECK(!query(argv[1]));
    puts("PASS peer kernel exec, fork and resident exec -> bootstrap/adoption -> maton ICD vkCreateInstance (test driver)");return 0;
}
