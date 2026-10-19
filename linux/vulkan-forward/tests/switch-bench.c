#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "runtime.h"
#include "backend.h"
/* Per-call adoption cost. Each component is measured on its own, warm, as the
 * median of RUNS runs, over >= 1e6 iterations. The 64-thread phase registers
 * 64 adopted threads and measures both one call amid 64 live bindings and 64
 * threads calling concurrently, to show the lookup is O(1): the per-call cost
 * must not grow with the number of live threads. */
#define ITERATIONS 1000000UL
#define WORKER_ITERATIONS 1000000UL
#ifndef WORKERS
#define WORKERS 64
#endif
#define RUNS 5
static volatile uintptr_t sink_u;
static volatile unsigned char sink_b;
static double now(void) {struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec+t.tv_nsec/1e9;}
/* Thread CPU time: unlike CLOCK_MONOTONIC it is not inflated when more threads
 * run than there are CPUs, so the 64-way phase reports real per-call work. */
static double now_cpu(void) {struct timespec t;assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t));return t.tv_sec+t.tv_nsec/1e9;}
/* Block a worker without burning a core: an adopted-thread futex, so 64 idle
 * registrations do not slow the single measuring thread. */
static void futex_wait(int *p,int v) {mv_sys(202,(long)p,0,v,0,0,0);}
static void futex_wake(int *p,int n) {mv_sys(202,(long)p,1,n,0,0,0);}
static double median(const double v[RUNS]) {
    double s[RUNS];
    for(int i=0;i<RUNS;i++) s[i]=v[i];
    for(int i=1;i<RUNS;i++) {double x=s[i];int j=i-1;while(j>=0 && s[j]>x){s[j+1]=s[j];j--;}s[j+1]=x;}
    return s[RUNS/2];
}
static double median_of(double (*run)(void *),void *arg) {
    double v[RUNS];
    for(int r=0;r<RUNS;r++) v[r]=run(arg);
    return median(v);
}
static double run_fs_read(void *arg) {
    int fast=*(int *)arg;uintptr_t x=0;double s=now();
    for(unsigned long i=0;i<ITERATIONS;i++) x+=mv_tp_read(fast);
    sink_u=x;return (now()-s)*1e9/ITERATIONS;
}
static double run_lookup(void *arg) {
    struct {struct adopt_api *api;uintptr_t fs;} *c=arg;struct adopt_thread *t=0;double s=now();
    for(unsigned long i=0;i<ITERATIONS;i++) t=c->api->lookup(c->fs);
    sink_b=(unsigned char)(t!=0);return (now()-s)*1e9/ITERATIONS;
}
static double run_fs_writes(void *arg) {
    uintptr_t *pair=arg;double s=now();
    for(unsigned long i=0;i<ITERATIONS;i++) {mv_tp_write(pair[0],1);mv_tp_write(pair[1],1);}
    double elapsed=now()-s;
    /* The loop leaves FS on the resident (pair[1]) base; put the calling
     * thread's own base back before anything else runs on it. */
    mv_tp_write(pair[0],1);
    return elapsed*1e9/ITERATIONS;
}
static double run_total(void *arg) {
    struct {struct adopt_api *api;int fast;} *c=arg;double s=now();
    for(unsigned long i=0;i<ITERATIONS;i++) {struct mv_scope scope;if(!switch_to_android(c->api,c->fast,&scope)){sink_b=0;return -1;}switch_back(&scope);}
    return (now()-s)*1e9/ITERATIONS;
}
struct worker {struct adopt_api *api;int fast;int *go;double ns;double wall;unsigned long iters;};
static void *worker_main(void *arg) {
    struct worker *w=arg;
    while(!__atomic_load_n(w->go,__ATOMIC_ACQUIRE)) futex_wait(w->go,0);
    double c0=now_cpu(),s=now();
    for(unsigned long i=0;i<w->iters;i++) {struct mv_scope scope;if(!switch_to_android(w->api,w->fast,&scope)){w->ns=-1;w->wall=-1;return 0;}switch_back(&scope);}
    w->ns=(now_cpu()-c0)*1e9/w->iters;
    w->wall=(now()-s)*1e9/w->iters;
    return 0;
}
int main(void) {
    setvbuf(stdout,0,_IONBF,0);
    struct adopt_api *api=(void *)mv_hex(mv_env("MATON_THREAD_ADOPT"));assert(api);
    int fast=mv_hex(mv_env("MATON_FAST_TLS"))!=0;
    printf("kernel auxv probe fast=%d; launcher bootstrap fast=%lu\n",mv_fast_tls(),(unsigned long)mv_hex(mv_env("MATON_FAST_TLS")));
    long fd=mv_open("/proc/self/auxv");uintptr_t pair[2];long bytes=fd<0?fd:mv_read(fd,pair,sizeof(pair));
    printf("auxv open=%ld first read=%ld\n",fd,bytes);if(fd>=0)mv_close(fd);
    assert(api->version>=1);
    uintptr_t fs=mv_tp_read(fast);
    printf("components (ns/call, %lu iterations, median of %d; FS base %#lx, v%u)\n",ITERATIONS,RUNS,(unsigned long)fs,api->version);
    printf("fs_read %s: %.1f\n",fast?"rdfsbase":"arch_prctl",median_of(run_fs_read,&fast));
    struct {struct adopt_api *api;uintptr_t fs;} lookup={api,fs};
    if(api->version>=2 && api->lookup)
        printf("lookup: %.1f\n",median_of(run_lookup,&lookup));
    struct adopt_thread *self=api->current();
    assert(self);
    if(fast) {uintptr_t writes[2]={fs,self->resident_fs};printf("fs_writes (2x wrfsbase): %.1f\n",median_of(run_fs_writes,writes));}
    struct {struct adopt_api *api;int fast;} call={api,fast};
    double one=median_of(run_total,&call);
    printf("total 1 thread: %.1f\n",one);
    /* Register `WORKERS` live adopted threads, then measure both one thread
     * amid them and all of them running concurrently. The workers block on a
     * futex until released, so the single-thread number isolates the lookup
     * from CPU contention: it must not grow with the number of live bindings. */
    pthread_t threads[WORKERS];struct worker workers[WORKERS];int go=0;
    printf("registering %d adopted threads\n",WORKERS);
    for(int i=0;i<WORKERS;i++) {workers[i]=(struct worker){api,fast,&go,0,0,WORKER_ITERATIONS};assert(!pthread_create(&threads[i],0,worker_main,&workers[i]));}
    printf("registered; measuring 1 thread amid them\n");
    double loaded=median_of(run_total,&call);
    printf("total 1 thread with %d live adopted threads: %.1f\n",WORKERS,loaded);
    __atomic_store_n(&go,1,__ATOMIC_RELEASE);
    futex_wake(&go,INT_MAX);
    double sum=0,sum_wall=0;for(int i=0;i<WORKERS;i++) {assert(!pthread_join(threads[i],0));assert(workers[i].ns>=0);sum+=workers[i].ns;sum_wall+=workers[i].wall;}
    double concurrent=sum/WORKERS,concurrent_wall=sum_wall/WORKERS;
    printf("total %d concurrent adopted threads: %.1f cpu, %.1f wall (mean per call)\n",WORKERS,concurrent,concurrent_wall);
    /* Machine-readable acceptance lines. */
    printf("ROUNDTRIP_1T %.2f\n",one);
    printf("ROUNDTRIP_1T_LOADED %.2f\n",loaded);
    printf("ROUNDTRIP_64T %.2f\n",concurrent);
    return 0;
}
