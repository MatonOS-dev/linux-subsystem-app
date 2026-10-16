#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "bridge.h"
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
    struct image image;
    if (proxy_load(argv[1], argv + 1, environ, &image)) return 4;
    /* No policy installation: isolates the loader/TLS experiment from seccomp.
     * Device needs root or existing Android policy allowing these syscalls. */
    proxy_enter(image.entry, image.stack);
}
