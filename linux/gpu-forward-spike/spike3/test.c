#define _GNU_SOURCE
#include <pthread.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/rseq.h>
#include <dirent.h>
#include <fenv.h>
#include "adopt.h"
#define N 32
extern void thunk_init(void),adopt_signal_gate(int);
extern long forwarded_identity(uintptr_t *,int);
static _Thread_local int marker;
static _Atomic int done,signals,release;
static uintptr_t identities[N];
static pthread_mutex_t robust;
static void *robust_worker(void *arg) {
    (void)arg; assert(!pthread_mutex_lock(&robust));
    pthread_exit((void *)42);
}
static void *nested(void *arg) {
    pthread_t child; void *v;
    assert(!pthread_create(&child,NULL,robust_worker,NULL));
    assert(!pthread_join(child,&v) && v==(void *)42);
    return arg;
}
void app_signal(int sig) { assert(sig==SIGUSR1 && marker==91); atomic_fetch_add(&signals,1); }
static void *worker(void *arg) {
    unsigned i=(uintptr_t)arg; marker=91;
    assert(fegetround()==FE_DOWNWARD);
    while(!atomic_load(&release)) sched_yield();
    for(int j=0;j<100;j++) {
        uintptr_t p;
        assert(forwarded_identity(&p,j==0)==syscall(SYS_gettid));
        if(!j) identities[i]=p; else assert(identities[i]==p);
    }
    atomic_fetch_add(&done,1);
    return arg;
}
static int kernel_threads(void) {
    DIR *dir=opendir("/proc/self/task"); assert(dir);
    int n=0; struct dirent *entry;
    while((entry=readdir(dir))) if(entry->d_name[0]!='.') n++;
    assert(!closedir(dir)); return n;
}
static pthread_t main_thread;
static void *main_joiner(void *arg) {
    (void)arg;
    void *value;
    assert(!pthread_join(main_thread,&value) && value==(void *)123);
    uintptr_t id; assert(forwarded_identity(&id,0)==syscall(SYS_gettid));
    puts("PASS: main pthread_exit, join main, surviving adopted child");
    return NULL;
}
static void *group_exiter(void *arg) {
    (void)arg; syscall(SYS_exit_group,73); __builtin_trap();
}
int main(int argc,char **argv) {
    thunk_init(); marker=91;
    assert(__rseq_size==0);
    assert(!fesetround(FE_DOWNWARD));
    if(argc>1) {
        pthread_t child;
        if(!strcmp(argv[1],"main-exit")) {
            main_thread=pthread_self();
            assert(!pthread_create(&child,NULL,main_joiner,NULL));
            pthread_exit((void *)123);
        }
        assert(!pthread_create(&child,NULL,group_exiter,NULL));
        for(;;) pause();
    }
    struct sigaction sa={0}; sa.sa_handler=adopt_signal_gate; sigemptyset(&sa.sa_mask);
    assert(!sigaction(SIGUSR1,&sa,NULL));
    uintptr_t main_id; assert(forwarded_identity(&main_id,1)==syscall(SYS_gettid));
    pthread_t threads[N];
    for(unsigned i=0;i<N;i++) assert(!pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)i));
    errno=0; assert(fork()==-1 && errno==ENOSYS);
    atomic_store(&release,1);
    for(unsigned i=0;i<N;i++) {void *v; assert(!pthread_join(threads[i],&v));assert((uintptr_t)v==i);}
    for(unsigned i=0;i<N;i++) { assert(identities[i]!=main_id); for(unsigned j=0;j<i;j++) assert(identities[i]!=identities[j]); }
    for(int batch=0;batch<30;batch++) {
        atomic_store(&done,0); pthread_attr_t attr; pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
        for(unsigned i=0;i<N;i++) assert(!pthread_create(&threads[i],&attr,worker,(void *)(uintptr_t)i));
        pthread_attr_destroy(&attr);
        while(atomic_load(&done)!=N) sched_yield();
        /* Allow resident destructors to finish before next batch. */
        usleep(1000);
    }
    pthread_mutexattr_t ma;
    assert(!pthread_mutexattr_init(&ma));
    assert(!pthread_mutexattr_setrobust(&ma,PTHREAD_MUTEX_ROBUST));
    assert(!pthread_mutex_init(&robust,&ma));
    assert(!pthread_mutexattr_destroy(&ma));
    pthread_t owner;
    assert(!pthread_create(&owner,NULL,nested,NULL));
    assert(!pthread_join(owner,NULL));
    assert(pthread_mutex_lock(&robust)==EOWNERDEAD);
    assert(!pthread_mutex_consistent(&robust));
    assert(!pthread_mutex_unlock(&robust)); assert(!pthread_mutex_destroy(&robust));
    for(int i=0;kernel_threads()!=1 && i<5000;i++) usleep(1000);
    assert(kernel_threads()==1);
    struct adopt_api *api=(void *)(uintptr_t)strtoull(getenv("MATON_THREAD_ADOPT"),NULL,16);
    unsigned clones=atomic_load(&api->clones),clone3s=atomic_load(&api->clone3s);
    assert((getenv("MATON_ADOPT_FORCE_CLONE") ? clones : clone3s)>=N+30*N+2);
    printf("clone=%u clone3=%u; nested creation, pthread_exit and robust owner death PASS\n",clones,clone3s);
    printf("PASS: %d joinable + %d detached, own resident tid/pthread_self, join, signal gate, fork ENOSYS; signals=%d\n",N,30*N,atomic_load(&signals));
}
