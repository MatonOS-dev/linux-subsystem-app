#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
static void *worker(void *unused) {
    (void)unused;stack_t ss;if(sigaltstack(NULL,&ss))return (void *)4;sigset_t set,got;sigfillset(&set);
    if(pthread_sigmask(SIG_SETMASK,&set,NULL)) return (void *)1;
    if(pthread_sigmask(SIG_SETMASK,NULL,&got) || !sigismember(&got,SIGSYS)) return (void *)2;
    errno=0;if(syscall(SYS_getpgrp)!=getpgid(0)) return (void *)3;
    return NULL;
}
int main(void) {
    for(int i=0;i<10000;i++) {
        pthread_t thread;
        if(pthread_create(&thread,NULL,worker,NULL)) return 1;
        void *r;if(pthread_join(thread,&r) || r) return 2;
    }
    FILE *maps=fopen("/proc/self/maps","r");if(!maps)return 3;
    size_t lines=0;char line[256];while(fgets(line,sizeof(line),maps))lines++;
    fclose(maps);
    if(lines>512)return 4;
    puts("PASS 10000 clone thread exits retain SIGSYS and keep mappings bounded");return 0;
}
