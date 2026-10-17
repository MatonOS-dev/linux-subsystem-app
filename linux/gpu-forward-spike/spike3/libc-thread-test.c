#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "adopt.h"
extern void thunk_init(void),adopt_signal_gate(int);
extern long forwarded_identity(uintptr_t *,int);
static _Thread_local int marker;
static _Atomic int ready,go,done,signals;
static uintptr_t ids[32];
void app_signal(int sig) {assert(sig==SIGUSR1 && marker==71);atomic_fetch_add(&signals,1);}
static void *work(void *arg) {
    unsigned i=(uintptr_t)arg;marker=71;atomic_fetch_add(&ready,1);
    while(!atomic_load(&go)) sched_yield();
    for(unsigned j=0;j<100;j++) {
        uintptr_t id;assert(forwarded_identity(&id,j==0)==syscall(SYS_gettid));
        assert(marker==71);if(!j) ids[i]=id;else assert(ids[i]==id);
    }
    atomic_fetch_add(&done,1);return arg;
}
int main(void) {
    thunk_init();marker=71;
    struct sigaction sa={0};sa.sa_handler=adopt_signal_gate;sigemptyset(&sa.sa_mask);
    assert(!sigaction(SIGUSR1,&sa,NULL));
    uintptr_t main_id;assert(forwarded_identity(&main_id,1)==syscall(SYS_gettid));
    pthread_t t[32];
    for(uintptr_t i=0;i<32;i++) assert(!pthread_create(&t[i],NULL,work,(void *)i));
    while(atomic_load(&ready)!=32) sched_yield();
    atomic_store(&go,1);
    for(uintptr_t i=0;i<32;i++) {void *v;assert(!pthread_join(t[i],&v) && (uintptr_t)v==i);}
    for(unsigned i=0;i<32;i++) {assert(ids[i]!=main_id);for(unsigned j=0;j<i;j++) assert(ids[i]!=ids[j]);}
    pthread_attr_t attr;assert(!pthread_attr_init(&attr));assert(!pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED));
    for(unsigned batch=0;batch<8;batch++) {
        atomic_store(&done,0);
        for(uintptr_t i=0;i<32;i++) assert(!pthread_create(&t[i],&attr,work,(void *)i));
        while(atomic_load(&done)!=32) sched_yield();
        usleep(1000);
    }
    assert(!pthread_attr_destroy(&attr));
    assert(atomic_load(&signals)==289);
    puts("PASS libc pthreads: 32 joinable + 256 detached, unique resident identities/TIDs, TLS, signal gate, join/exit");
}
