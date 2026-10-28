#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifdef RESIDENT_PTHREAD_DSO
#include <pthread.h>
int resident_spawn_worker(pthread_t *t,const pthread_attr_t *a,void *(*s)(void *),void *p){return pthread_create(t,a,s,p);}
#else
#include "../resident-pthread.h"
int proxy_resident_signals_begin(uint64_t *previous){
    uint64_t block=UINT64_C(1)<<(SIGCHLD-1);
    return (int)syscall(SYS_rt_sigprocmask,SIG_BLOCK,&block,previous,8);
}
int proxy_resident_signals_end(uint64_t previous){
    return (int)syscall(SYS_rt_sigprocmask,SIG_SETMASK,&previous,NULL,8);
}
static void *worker(void *unused){
    (void)unused;sigset_t mask;
    assert(!pthread_sigmask(SIG_SETMASK,NULL,&mask));
    assert(sigismember(&mask,SIGCHLD));return NULL;
}
int main(int argc,char **argv){
    assert(argc==2);
    void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(library);
    resident_pthread_create_fn create=(resident_pthread_create_fn)dlsym(library,"resident_spawn_worker");assert(create);
    sigset_t mask,old;sigemptyset(&mask);sigaddset(&mask,SIGCHLD);
    assert(!pthread_sigmask(SIG_UNBLOCK,&mask,&old));
    pthread_t child;assert(!pthread_create(&child,NULL,worker,NULL));
    assert(!pthread_join(child,NULL));
    assert(!create(&child,NULL,worker,NULL));assert(!pthread_join(child,NULL));
    assert(!pthread_sigmask(SIG_SETMASK,NULL,&mask));assert(!sigismember(&mask,SIGCHLD));
    assert(!pthread_sigmask(SIG_SETMASK,&old,NULL));
    puts("PASS lazy resident pthread inherits blocked SIGCHLD; creator mask restored");
}

#endif
