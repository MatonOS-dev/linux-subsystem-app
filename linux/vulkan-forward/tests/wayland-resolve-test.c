#define _GNU_SOURCE
#include "../wayland_resolve.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include "../../gpu-bridge-common/bootstrap.h"

int main(void) {
    void *handle=dlopen("libwayland-client.so.0",RTLD_NOW|RTLD_LOCAL);
    assert(handle);
    assert(dlsym(handle,"wl_display_connect"));
    assert(dlsym(handle,"wl_proxy_marshal_flags"));
    assert(mv_wayland_symbol("wl_display_connect")==dlsym(handle,"wl_display_connect"));
    assert(mv_wayland_symbol("wl_proxy_marshal_flags")==dlsym(handle,"wl_proxy_marshal_flags"));
    /* This request is generated inline in wayland-client-protocol.h. A mock
     * exporting it used to hide a fatal production helper dependency. */
    assert(!dlsym(handle,"wl_display_get_registry"));
    assert(!mv_wayland_symbol("wl_display_get_registry"));
    assert(!mv_wayland_symbol("mv_symbol_that_does_not_exist"));
    /* Model the resident loader: the payload has its own program headers
     * and DT_DEBUG, independent of the kernel auxv. */
    struct link_map *map=0;
    assert(!dlinfo(handle,RTLD_DI_LINKMAP,&map));
    struct r_debug debug={.r_version=1,.r_map=map};
    Elf64_Dyn dynamic[]={{.d_tag=DT_DEBUG,.d_un.d_ptr=(Elf64_Addr)&debug},{.d_tag=DT_NULL}};
    Elf64_Phdr headers[2]={
        {.p_type=PT_PHDR},
        {.p_type=PT_DYNAMIC,.p_vaddr=(Elf64_Addr)dynamic}
    };
    headers[0].p_vaddr=(Elf64_Addr)headers;
    struct maton_gpu_bootstrap *bootstrap=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(bootstrap!=MAP_FAILED);
    bootstrap->magic=MATON_GPU_BOOTSTRAP_MAGIC;
    snprintf(bootstrap->phdr,sizeof(bootstrap->phdr),"%lx",(unsigned long)headers);
    snprintf(bootstrap->phnum,sizeof(bootstrap->phnum),"%x",2);
    assert(!prctl(PR_SET_VMA,PR_SET_VMA_ANON_NAME,bootstrap,4096,MATON_GPU_BOOTSTRAP_NAME));
    assert(!mprotect(bootstrap,4096,PROT_READ));
    assert(mv_wayland_symbol("wl_proxy_marshal_flags")==dlsym(handle,"wl_proxy_marshal_flags"));
    /* Intercepted exec must replace the initial launcher's metadata. */
    assert(!mprotect(bootstrap,4096,PROT_READ|PROT_WRITE));
    maton_gpu_payload(bootstrap,(uintptr_t)headers,2);
    assert(!mprotect(bootstrap,4096,PROT_READ));
    assert(mv_wayland_symbol("wl_proxy_get_version")==dlsym(handle,"wl_proxy_get_version"));
    assert(!munmap(bootstrap,4096));
    dlclose(handle);
    puts("PASS Wayland resolver: loaded library symbols via ELF dynamic tables");
}
