#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/auxv.h>
#include <elf.h>
#include "bridge.h"
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
    shadow_publish();
#ifdef MATON_ADOPT
    if (adopt_install()) return 5;
#endif
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
    /* No policy installation: isolates the loader/TLS experiment from seccomp.
     * Device needs root or existing Android policy allowing these syscalls. */
    proxy_enter(image.entry, image.stack);
}
