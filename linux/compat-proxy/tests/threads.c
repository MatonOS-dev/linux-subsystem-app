#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
static void *worker(void *unused) {
    (void)unused;stack_t ss;if(sigaltstack(NULL,&ss))return (void *)4;sigset_t set,got;sigfillset(&set);
    if(pthread_sigmask(SIG_SETMASK,&set,NULL)) return (void *)1;
    if(pthread_sigmask(SIG_SETMASK,NULL,&got) || sigismember(&got,SIGSYS)) return (void *)2;
    errno=0;if(syscall(SYS_getpgrp)!=getpgid(0)) return (void *)3;
    return NULL;
}
int main(void) {
    pthread_t threads[16];
    for(int round=0;round<10;round++){
    for(int i=0;i<16;i++) if(pthread_create(&threads[i],NULL,worker,NULL)) return 1;
    for(int i=0;i<16;i++) {void *r;if(pthread_join(threads[i],&r) || r) return 2;}
    }
    puts("PASS 160 clone threads retain deliverable SIGSYS and virtual altstacks");return 0;
}
