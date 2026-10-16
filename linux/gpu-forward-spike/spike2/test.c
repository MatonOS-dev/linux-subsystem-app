#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "switch.h"
#include "client.h"
#include "thunks.h"
#include <vulkan/vulkan.h>
static _Thread_local volatile int marker;
static uintptr_t app_fs, foreign_fs;
static void *callback_data;
static volatile sig_atomic_t signaled;
static unsigned char foreign[4096] __attribute__((aligned(4096)));
/* No compiler TLS accesses until the entry gate has restored app FS. A real
 * proxy must look up app_fs by raw gettid, not this single-thread test global. */
static void app_handler(int sig) { assert(sig == SIGUSR1); assert(marker == 91); signaled++; }
static void signal_gate(int sig) {
    uintptr_t interrupted = gs_read();
    gs_write(app_fs);
    app_handler(sig);
    gs_write(interrupted);
}
static VKAPI_ATTR void *VKAPI_CALL app_allocate(void *p, size_t n, size_t a, VkSystemAllocationScope scope) {
    assert(gs_read() == app_fs && marker == 91);
    assert(p == &marker && n == 42 && a == 16 && scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    return p;
}
/* Reverse thunk prototype for VkAllocationCallbacks. Production needs retained
 * callback contexts per instance/device, not one global callback pointer. */
static __attribute__((noinline)) VKAPI_ATTR void *VKAPI_CALL reverse_allocate(void *p, size_t n, size_t a, VkSystemAllocationScope scope) {
    uintptr_t saved = gs_read();
    gs_write(app_fs);
    PFN_vkAllocationFunction volatile callback = app_allocate;
    void *result = callback(p,n,a,scope);
    gs_write(saved);
    return result;
}
static VKAPI_ATTR VkBool32 VKAPI_CALL app_debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    assert(gs_read() == app_fs && marker == 91);
    assert(severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT);
    assert(types == VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT && data->messageIdNumber == 35 && user == callback_data);
    return VK_TRUE;
}
static __attribute__((noinline)) VkBool32 reverse_debug(const VkDebugUtilsMessengerCallbackDataEXT *data) {
    uintptr_t saved = gs_read();
    gs_write(app_fs);
    PFN_vkDebugUtilsMessengerCallbackEXT volatile callback = app_debug;
    VkBool32 result = callback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, data, callback_data);
    gs_write(saved);
    return result;
}
static VkResult fake_enumerate(uint32_t *count, VkLayerProperties *properties) {
    if (gs_read() != foreign_fs || properties) __builtin_trap();
    if (reverse_allocate(callback_data,42,16,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT) != callback_data) __builtin_trap();
    VkDebugUtilsMessengerCallbackDataEXT data = {.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT, .messageIdNumber=35};
    if (reverse_debug(&data) != VK_TRUE || gs_read() != foreign_fs) __builtin_trap();
    /* The target never references compiler TLS; use raw tgkill on this thread. */
    if (gs_raw(234, gs_raw(39,0,0,0), gs_raw(186,0,0,0), SIGUSR1)) __builtin_trap();
    if (gs_read() != foreign_fs) __builtin_trap();
    *count = 17;
    return VK_SUCCESS;
}
struct maton_call maton_enter(const char *name) {
    assert(name && gs_read() == app_fs);
    struct maton_call call = {gs_read(), (void (*)(void))fake_enumerate};
    gs_write(foreign_fs);
    return call;
}
void maton_leave(struct maton_call *call) { gs_write(call->previous); }
extern VkResult maton_vkEnumerateInstanceVersion(uint32_t *);
extern VkResult maton_vkEnumerateInstanceLayerProperties(uint32_t *, VkLayerProperties *);
/* Use the registry-generated two-argument thunk; ABI-compatible fake target. */
static void *worker(void *arg) {
    (void)arg;
    marker = 123;
    uintptr_t original = gs_read();
    struct shadow_lease *lease = shadow_current();
    assert(lease && lease == shadow_current() && lease->fs != original);
    gs_write(lease->fs);
    uintptr_t donor_fs = gs_read();
    gs_write(original);
    assert(donor_fs == lease->fs && marker == 123);
    return NULL; /* glibc key destructor frees/join donor */
}
int main(int argc, char **argv) {
    gs_init(argc > 1 && argv[1][0] == 'f');
    app_fs = gs_read(); foreign_fs = (uintptr_t)foreign; marker = 91; callback_data = (void *)&marker;
    struct sigaction action = {.sa_handler=signal_gate};
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGUSR1,&action,NULL));
    for (int i=0;i<1000;i++) {
        uint32_t count = 0;
        assert(maton_vkEnumerateInstanceLayerProperties(&count,NULL) == VK_SUCCESS);
        assert(count == 17 && gs_read() == app_fs && marker == 91);
    }
    assert(signaled == 1000);
    if (getenv("MATON_GPU_SHADOW")) {
        for (int round=0;round<10;round++) {
            pthread_t threads[4];
            for (int i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,worker,NULL));
            for (int i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
        }
    }
    printf("PASS %s: generated thunk, FS restoration, reverse allocator/debug, signal-mid-call%s\n",
           gs_fsgsbase ? "WRFSBASE" : "arch_prctl",
           getenv("MATON_GPU_SHADOW") ? ", 40 lazy donor lifecycles" : "");
}
