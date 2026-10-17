#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/auxv.h>
#include <elf.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include "bridge.h"
#include "bootstrap.h"
#ifdef MATON_ADOPT
#include "spike3/adopt.h"
#endif
#include "spike2/provider.h"
#include "../compat-proxy/proxy.h"
extern char **environ;
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    static struct gpu_bridge bridge;
    bridge.version = 1;
    bridge.owner = syscall(SYS_gettid);
    if (syscall(SYS_arch_prctl, 0x1003, &bridge.tls)) return 3;
    bridge.open = dlopen;
    bridge.symbol = dlsym;
    char value[32];
    snprintf(value, sizeof(value), "%lx", (unsigned long)&bridge);
    setenv("MATON_GPU_BRIDGE", value, 1);
#ifdef MATON_ADOPT
    if (adopt_install()) return 5;
#else
    shadow_publish();
#endif
    /* Discoverable without importing the application's libc data symbols.
     * Read-only, process-local capability publication for freestanding ICDs. */
    struct maton_gpu_bootstrap *bootstrap = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (bootstrap == MAP_FAILED) return 6;
    bootstrap->magic=MATON_GPU_BOOTSTRAP_MAGIC;
    snprintf(bootstrap->bridge,sizeof(bootstrap->bridge),"%s",getenv("MATON_GPU_BRIDGE"));
    snprintf(bootstrap->adopt,sizeof(bootstrap->adopt),"%s",getenv("MATON_QUERY_NO_ADOPT") ? "0" : (getenv("MATON_THREAD_ADOPT") ? getenv("MATON_THREAD_ADOPT") : "0"));
    snprintf(bootstrap->fast,sizeof(bootstrap->fast),"%lx",getauxval(AT_HWCAP2)&2);
    snprintf(bootstrap->wsi,sizeof(bootstrap->wsi),"%s",getenv("MATON_WSI_HELPER") ? getenv("MATON_WSI_HELPER") : "0");
    if (prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, bootstrap, 4096,
              MATON_GPU_BOOTSTRAP_NAME) || mprotect(bootstrap,4096,PROT_READ)) return 6;
    struct image image;
    if (proxy_load(argv[1], argv + 1, environ, &image)) return 4;
    /* PROXY_LIBRARY has no entry-stack auxv import. Preserve the kernel's
     * FSGSBASE permission bit in this isolated launcher's generated stack. */
    uintptr_t *cursor = (uintptr_t *)image.stack;
    uintptr_t count = *cursor++;
    cursor += count + 1;
    while (*cursor) cursor++;
    Elf64_auxv_t *aux = (void *)(cursor + 1);
    for (; aux->a_type; aux++)
        if (aux->a_type == AT_HWCAP2) aux->a_un.a_val = getauxval(AT_HWCAP2);
    /* Adoption uses per-thread user dispatch, which resets on exec. Other
     * variants retain the original unfiltered loader/TLS experiment. */
#ifdef MATON_ADOPT
    adopt_launch(image.entry, image.stack);
#else
    proxy_enter(image.entry, image.stack);
#endif
}
