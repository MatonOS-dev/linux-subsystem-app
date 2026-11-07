#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "runtime.h"
#include "backend.h"
static void *worker(void *arg) {
    (void)arg;
    for(unsigned i=0;i<1000;i++) {
        size_t n=1+i*19;unsigned char *p=mv_alloc(n);assert(p);
        assert((uintptr_t)p%_Alignof(max_align_t)==0);
        memset(p,0xa5,n);for(size_t j=0;j<n;j++)assert(p[j]==0xa5);
        unsigned char *q=mv_alloc(n);assert(q);memcpy(q,p,n);assert(!memcmp(p,q,n));
        q[n-1]=0;assert(memcmp(p,q,n)>0);mv_free(q);mv_free(p);
    }
    return 0;
}
int main(void) {
    assert(!mv_alloc(SIZE_MAX));mv_free(0);
    assert(!strcmp("same","same"));assert(strcmp("a","b")<0);assert(strcmp("aa","a")>0);
    assert(mv_hex("123abc")==0x123abc);assert(!mv_hex("invalid"));assert(!mv_hex("fffffffffffffffff"));
    pthread_t threads[8];for(unsigned i=0;i<8;i++)assert(!pthread_create(&threads[i],0,worker,0));
    for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],0));
    uintptr_t fs=mv_tp_read(0);mv_tp_write(fs,0);assert(mv_tp_read(0)==fs);
    if(mv_fast_tls()){assert(mv_tp_read(1)==fs);mv_tp_write(fs,1);}
    struct mv_scope scope;assert(!switch_to_android(0,0,&scope));
    puts("PASS runtime: helpers, overflow, alignment, 8 threads x 1000 allocations, TLS backends, fail closed");
}
