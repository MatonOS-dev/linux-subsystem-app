#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>

int main(void){
    void *handle=dlopen("./build/site-rewrite-fde.so",RTLD_NOW|RTLD_LOCAL);
    assert(handle);
    long (*call_site)(void)=dlsym(handle,"site_fde_call");
    void *(*address_site)(void)=dlsym(handle,"site_fde_address");
    assert(call_site && address_site);
    unsigned char *site=address_site();
    assert(site[0]==0xb8 && site[5]==0xff && site[6]==0xd0);
    (void)call_site();
    assert(!dlclose(handle));
    puts("PASS FDE-only range rewrites its syscall without symbol-table scanning");
}
