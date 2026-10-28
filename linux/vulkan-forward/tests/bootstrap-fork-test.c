/* Fresh ICD discovery after fork must use the inherited named mapping.
 * The host Vulkan loader supplies a real CPU driver; the bridge/adoption
 * boundary uses the host TLS on both sides in this host-only fixture. */
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "../runtime.h"
#include "../arch/x86_64/backend.h"
#include "../../gpu-bridge-common/bootstrap.h"
#include "../../gpu-bridge-common/bridge.h"
static struct adopt_thread thread;
static struct adopt_thread *current(void) {thread.resident_fs=mv_tp_read(0);return &thread;}
static struct adopt_api adoption={.version=2,.current=current};
static void *open_library(const char *name,int flags) {
    (void)flags;
    return strstr(name,"libvulkan")?dlopen("libvulkan.so.1",RTLD_NOW|RTLD_LOCAL):NULL;
}
static const char *last_error(void) {return dlerror();}
static struct gpu_bridge bridge={.version=2,.open=open_library,.symbol=dlsym,.error=last_error};
int main(int argc,char **argv) {
    assert(argc==2);
    struct maton_gpu_bootstrap *b=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(b!=MAP_FAILED);b->magic=MATON_GPU_BOOTSTRAP_MAGIC;
    snprintf(b->bridge,sizeof(b->bridge),"%lx",(unsigned long)&bridge);
    snprintf(b->adopt,sizeof(b->adopt),"%lx",(unsigned long)&adoption);
    assert(!prctl(PR_SET_VMA,PR_SET_VMA_ANON_NAME,b,4096,MATON_GPU_BOOTSTRAP_NAME));
    assert(!mprotect(b,4096,PROT_READ));
    pid_t child=fork();assert(child>=0);
    if(!child) {
        /* Load for the first time in the child: no cached bootstrap pointer. */
        void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
        PFN_vkGetInstanceProcAddr gipa=(void *)dlsym(lib,"vk_icdGetInstanceProcAddr");assert(gipa);
        PFN_vkCreateInstance create=(void *)gipa(NULL,"vkCreateInstance");assert(create);
        VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        VkInstance instance=NULL;assert(create(&info,NULL,&instance)==VK_SUCCESS && instance);
        PFN_vkDestroyInstance destroy=(void *)gipa(instance,"vkDestroyInstance");assert(destroy);
        destroy(instance,NULL);_exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child && status==0);
    assert(!munmap(b,4096));
    puts("PASS fork -> fresh maton Vulkan ICD -> inherited bootstrap -> vkCreateInstance");
}
