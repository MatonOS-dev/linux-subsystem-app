#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
extern long lazy_open(long,const char *,long,long);
extern long lazy_park(long,const char *,long,long);
extern long lazy_read(long,const char *,long,long);
extern unsigned char lazy_syscall[];
_Atomic int lazy_phase;
static void parked(int sig,siginfo_t *info,void *ctx){
    (void)sig;(void)info;ucontext_t *uc=ctx;
    assert(uc->uc_mcontext.gregs[REG_RAX]==257);
    uc->uc_mcontext.gregs[REG_RIP]=(uintptr_t)lazy_syscall;
    atomic_store(&lazy_phase,2);
    while(atomic_load(&lazy_phase)!=3) __asm__ volatile("pause");
}
static void *worker(void *unused){
    (void)unused;long fd=lazy_park(AT_FDCWD,"/etc/hosts",O_RDONLY,0);
    assert(fd>=0);close(fd);return NULL;
}
int main(int argc,char **argv){
    assert(argc==2);int enabled=atoi(argv[1]);
    assert(lazy_syscall[0]==0x0f && lazy_syscall[1]==5);
    struct sigaction action={.sa_sigaction=parked,.sa_flags=SA_SIGINFO};
    assert(!sigaction(SIGUSR1,&action,NULL));
    pthread_t thread;assert(!pthread_create(&thread,NULL,worker,NULL));
    while(atomic_load(&lazy_phase)!=1)__asm__ volatile("pause");
    assert(!pthread_kill(thread,SIGUSR1));
    while(atomic_load(&lazy_phase)!=2)__asm__ volatile("pause");
    long fd=lazy_open(AT_FDCWD,"/etc/hosts",O_RDONLY,0);assert(fd>=0);close(fd);
    assert(lazy_syscall[0]==(enabled?0xff:0x0f));
    atomic_store(&lazy_phase,3);assert(!pthread_join(thread,NULL));
    fd=lazy_read(AT_FDCWD,"/etc/hosts",O_RDONLY,0);assert(fd>=0);close(fd);
    const unsigned char *read_site=(const void *)lazy_read;
    assert(read_site[8]==0x0f && read_site[9]==5);
    pid_t child=fork();assert(child>=0);
    if(!child)abort();
    int status;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
    puts("PASS lazy non-adjacent, RAX-read rejection, forced old-number race, fatal registry");
}
