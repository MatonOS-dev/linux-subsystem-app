#define _GNU_SOURCE
#include "../wayland_resolve.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>

int main(void) {
    void *handle=dlopen("libwayland-client.so.0",RTLD_NOW|RTLD_LOCAL);
    assert(handle);
    assert(dlsym(handle,"wl_display_connect"));
    assert(dlsym(handle,"wl_proxy_marshal_flags"));
    assert(mv_wayland_symbol("wl_display_connect")==dlsym(handle,"wl_display_connect"));
    assert(mv_wayland_symbol("wl_proxy_marshal_flags")==dlsym(handle,"wl_proxy_marshal_flags"));
    assert(!mv_wayland_symbol("mv_symbol_that_does_not_exist"));
    dlclose(handle);
    puts("PASS Wayland resolver: loaded library symbols via ELF dynamic tables");
}
