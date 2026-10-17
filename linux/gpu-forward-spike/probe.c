#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "bridge.h"
#include <vulkan/vulkan.h>
/* No libc calls or TLS references while the foreign FS base is installed. */
static inline long raw(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}
static struct gpu_bridge *bridge;
static void *resolve(const char *lib, const char *name) {
    if (raw(SYS_gettid, 0, 0) != bridge->owner) return NULL;
    uintptr_t saved;
    if (raw(SYS_arch_prctl, 0x1003, (long)&saved)) return NULL;
    if (raw(SYS_arch_prctl, 0x1002, bridge->tls)) return NULL;
    void *h = bridge->open(lib, RTLD_NOW | RTLD_LOCAL);
    void *p = h ? bridge->symbol(h, name) : NULL;
    if (raw(SYS_arch_prctl, 0x1002, saved)) __builtin_trap();
    return p;
}
static void *thread(void *unused) {
    (void)unused;
    /* x86 bionic THREAD_ID is FS+8. glibc uses that slot for its DTV. */
    uintptr_t slot;
    __asm__("movq %%fs:8,%0" : "=r"(slot));
    printf("glibc child: FS+8=%lx; foreign owner correctly refused=%d\n",
           (unsigned long)slot, resolve("libc.so.6", "getpid") == NULL);
    return NULL;
}
int main(int argc, char **argv) {
    const char *s = getenv("MATON_GPU_BRIDGE");
    if (!s) return 2;
    bridge = (void *)(uintptr_t)strtoull(s, NULL, 16);
    if (bridge->version != 1) return 3;
    const char *lib = argc > 1 ? argv[1] : "libc.so.6";
    const char *sym = argc > 2 ? argv[2] : "getpid";
    void *p = resolve(lib, sym);
    printf("foreign dlopen/dlsym %s:%s=%p\n", lib, sym, p);
    if (!p) return 4;
    /* getpid has no callbacks/allocator ABI; EGL/Vulkan pointers deliberately
     * aren't called on glibc TLS. Resolve alone does not prove driver safety. */
    if (!strcmp(sym, "getpid")) {
        uintptr_t saved;
        raw(SYS_arch_prctl, 0x1003, (long)&saved);
        raw(SYS_arch_prctl, 0x1002, bridge->tls);
        int pid = ((int (*)(void))p)();
        raw(SYS_arch_prctl, 0x1002, saved);
        if (pid != getpid()) return 5;
    }
    if (argc > 2 && !strcmp(sym, "eglBindAPI")) {
        void *(*display)(void *) = resolve(lib,"eglGetDisplay");
        unsigned (*initialize)(void *,int *,int *) = resolve(lib,"eglInitialize");
        unsigned (*terminate)(void *) = resolve(lib,"eglTerminate");
        int (*error)(void) = resolve(lib,"eglGetError");
        if (!display || !initialize || !terminate || !error) return 8;
        uintptr_t saved;
        raw(SYS_arch_prctl, 0x1003, (long)&saved);
        raw(SYS_arch_prctl, 0x1002, bridge->tls);
        void *d = display(NULL);
        int major=0,minor=0;
        unsigned initialized = initialize(d,&major,&minor);
        unsigned bound = initialized ? ((unsigned (*)(unsigned))p)(argc>3 ? 0x30A2 : 0x30A0) : 0;
        int e = error();
        if (initialized) terminate(d);
        raw(SYS_arch_prctl, 0x1002, saved);
        printf("foreign EGL %d.%d initialize=%u bind=%u error=%#x\n",major,minor,initialized,bound,e);
        if (!initialized || !bound) return 8;
    }
    if (argc > 2 && !strcmp(sym, "vkGetInstanceProcAddr")) {
        PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)p;
        uintptr_t saved;
        raw(SYS_arch_prctl, 0x1003, (long)&saved);
        raw(SYS_arch_prctl, 0x1002, bridge->tls);
        PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL,"vkCreateInstance");
        VkInstanceCreateInfo info = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        VkInstance instance = VK_NULL_HANDLE;
        VkResult result = create ? create(&info,NULL,&instance) : VK_ERROR_INITIALIZATION_FAILED;
        if (result == VK_SUCCESS) {
            PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(instance,"vkDestroyInstance");
            destroy(instance,NULL);
        }
        raw(SYS_arch_prctl, 0x1002, saved);
        printf("foreign Vulkan create/destroy (direct resident loader): %d\n", result);
        if (result != VK_SUCCESS) return 7;
    }
    if (!strcmp(sym, "getpid")) {
        const int iterations = 100000;
        struct timespec begin, end;
        clock_gettime(CLOCK_MONOTONIC, &begin);
        for (int i=0;i<iterations;i++) {
            uintptr_t saved;
            raw(SYS_arch_prctl, 0x1003, (long)&saved);
            raw(SYS_arch_prctl, 0x1002, bridge->tls);
            ((int (*)(void))p)();
            raw(SYS_arch_prctl, 0x1002, saved);
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        double switched = ((end.tv_sec-begin.tv_sec)*1e9+end.tv_nsec-begin.tv_nsec)/iterations;
        clock_gettime(CLOCK_MONOTONIC, &begin);
        for (int i=0;i<iterations;i++) getpid();
        clock_gettime(CLOCK_MONOTONIC, &end);
        double native = ((end.tv_sec-begin.tv_sec)*1e9+end.tv_nsec-begin.tv_nsec)/iterations;
        printf("getpid ns/call: native=%.1f switch=%.1f delta=%.1f n=%d\n",native,switched,switched-native,iterations);
    }
    pthread_t t;
    if (pthread_create(&t, NULL, thread, NULL) || pthread_join(t, NULL)) return 6;
    return 0;
}
