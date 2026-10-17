#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include "runtime.h"
#include "backend.h"
static double now(void) {struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec+t.tv_nsec/1e9;}
int main(void) {
    struct adopt_api *api=(void *)mv_hex(mv_env("MATON_THREAD_ADOPT"));assert(api);
    printf("kernel auxv probe fast=%d; launcher bootstrap fast=%lu\n",mv_fast_tls(),(unsigned long)mv_hex(mv_env("MATON_FAST_TLS")));
    long fd=mv_open("/proc/self/auxv");uintptr_t pair[2];long bytes=fd<0?fd:mv_read(fd,pair,sizeof(pair));
    printf("auxv open=%ld first read=%ld\n",fd,bytes);if(fd>=0)mv_close(fd);
    for(int fast=0;fast<=(mv_hex(mv_env("MATON_FAST_TLS"))!=0);fast++) {
        double start=now();
        for(unsigned j=0;j<100000;j++) {struct mv_scope s;assert(switch_to_android(api,fast,&s));switch_back(&s);}
        printf("TLS round trip (%s, adoption lookup + selector included): %.1f ns/call, 100000 calls\n",fast?"WRFSBASE":"arch_prctl",(now()-start)*1e9/100000);
    }
}
