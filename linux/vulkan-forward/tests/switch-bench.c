#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include "runtime.h"
#include "backend.h"
static double now(void) {struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec+t.tv_nsec/1e9;}
int main(void) {
    struct adopt_api *api=(void *)mv_hex(mv_env("MATON_THREAD_ADOPT"));assert(api);
    for(int fast=0;fast<=mv_fast_tls();fast++) {
        double start=now();
        for(unsigned j=0;j<100000;j++) {struct mv_scope s;assert(switch_to_android(api,fast,&s));switch_back(&s);}
        printf("TLS round trip (%s, adoption lookup + selector included): %.1f ns/call, 100000 calls\n",fast?"WRFSBASE":"arch_prctl",(now()-start)*1e9/100000);
    }
}
