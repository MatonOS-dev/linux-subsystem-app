#define _GNU_SOURCE
/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <linux/futex.h>
#include <sys/mount.h>
#include <sched.h>
extern char **environ;
static _Atomic int parked,busy,racers;
static int futex_word;
static void *busy_worker(void *arg){(void)arg;atomic_store(&busy,1);for(;;)__asm__ volatile("pause");return NULL;}
static void *exec_worker(void *arg){(void)arg;atomic_store(&parked,1);for(;;)syscall(SYS_futex,&futex_word,FUTEX_WAIT,0,NULL,NULL,0);return NULL;}
static void *exit_racer(void *arg){(void)arg;atomic_fetch_add(&racers,1);while(atomic_load(&racers)<2)sched_yield();syscall(SYS_exit_group,37);return NULL;}
static void *exit_worker(void *arg){(void)arg;syscall(SYS_exit_group,37);return NULL;}
static void *worker(void *arg){puts("inprocess-worker");return arg;}
int main(int argc,char **argv) {
    if(argc<2 || strcmp(argv[1],"fixture") || !getenv("INPROCESS_TEST") || strcmp(getenv("INPROCESS_TEST"),"present"))return 20;
    if(argc>2 && !strcmp(argv[2],"fault")){*(volatile int *)0=1;return 52;}
    if(access("/",F_OK))return 22;
    if(getenv("RESIDENT_MAP")) {
        void *foreign=(void *)strtoul(getenv("RESIDENT_MAP"),NULL,16);
        errno=0;if(munmap(foreign,4096)!=-1 || errno!=EPERM)return 40;
        errno=0;if(mprotect(foreign,4096,PROT_NONE)!=-1 || errno!=EPERM)return 41;
        errno=0;if(mmap(foreign,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=MAP_FAILED || errno!=EPERM)return 42;
        errno=0;if(close(atoi(getenv("RESIDENT_FD")))!=-1 || errno!=EBADF)return 43;
        char cwd[32];if(chdir("/") || !getcwd(cwd,sizeof(cwd)) || strcmp(cwd,"/"))return 44;
        if(unshare(CLONE_NEWNS) || mount("/","/resident-test-view",NULL,MS_BIND,NULL) || access("/resident-test-view/dev/null",F_OK) || umount2("/resident-test-view",0))return 48;
        umask(077);
        struct rlimit limit={32,32};if(setrlimit(RLIMIT_NOFILE,&limit))return 45;
    }
    /* The inherited-filter host run also verifies unsupported raw retries. */
    errno=0;
    long priority=syscall(SYS_getpriority,0,0);
    if(priority==-1 && errno!=ENOSYS)return 23;
    printf("inprocess-payload pid=%d argv=%s env=%s\n",(int)getpid(),argv[1],getenv("INPROCESS_TEST"));
    pthread_t thread;void *answer;
    if(pthread_create(&thread,NULL,worker,(void *)42) || pthread_join(thread,&answer) || answer!=(void *)42)return 21;
    if(argc>2 && !strcmp(argv[2],"group-race")) {
        pthread_t a,b;
        if(pthread_create(&a,NULL,exit_racer,NULL) || pthread_create(&b,NULL,exit_racer,NULL))return 53;
        for(;;)sched_yield();
    }
    if(argc>2 && (!strcmp(argv[2],"group-main") || !strcmp(argv[2],"group-worker") || !strcmp(argv[2],"thread-main"))) {
        pthread_t waiting,spinning;
        if(pthread_create(&waiting,NULL,exec_worker,NULL) || pthread_create(&spinning,NULL,busy_worker,NULL))return 50;
        while(!atomic_load(&parked) || !atomic_load(&busy))sched_yield();
        if(!strcmp(argv[2],"group-main"))syscall(SYS_exit_group,37);
        pthread_t exiting;if(pthread_create(&exiting,NULL,exit_worker,NULL))return 51;
        if(!strcmp(argv[2],"thread-main"))syscall(SYS_exit,0);
        for(;;)syscall(SYS_futex,&futex_word,FUTEX_WAIT,0,NULL,NULL,0);
    }
    if(argc>2 && !strcmp(argv[2],"exec0")) {
        if(argc!=7)return 30;
        pthread_t waiting;if(pthread_create(&waiting,NULL,exec_worker,NULL))return 46;
        pthread_t spinning;if(pthread_create(&spinning,NULL,busy_worker,NULL))return 49;
        while(!atomic_load(&parked) || !atomic_load(&busy))sched_yield();
        char *bad[]={"/does-not-exist",NULL};
        errno=0;if(execve(bad[0],bad,environ)!=-1 || errno!=ENOENT)return 47;
        int keep=open("/dev/null",O_RDONLY),drop=open("/dev/null",O_RDONLY|O_CLOEXEC);
        if(keep<0 || drop<0)return 31;
        char a[24],b[24],pid[24];snprintf(a,sizeof(a),"%d",keep);snprintf(b,sizeof(b),"%d",drop);snprintf(pid,sizeof(pid),"%d",(int)getpid());
        char *next[]={argv[3],"--inhibit-cache","--library-path",argv[4],argv[5],"fixture","exec1",a,b,pid,argv[6],NULL};
        execve(next[0],next,environ);return 32;
    }
    if(argc>2 && (!strcmp(argv[2],"exec1") || !strcmp(argv[2],"exec2"))) {
        if(argc<6 || getpid()!=atoi(argv[5]) || fcntl(atoi(argv[3]),F_GETFD)<0)return 33;
        errno=0;if(fcntl(atoi(argv[4]),F_GETFD)!=-1 || errno!=EBADF)return 34;
        if(!strcmp(argv[2],"exec1")) {
            if(argc!=7)return 35;
            char *next[]={argv[6],"fixture","exec2",argv[3],argv[4],argv[5],NULL};
            execve(next[0],next,environ);return 36;
        }
        puts("exec-chain argv/env/fds/pid preserved");
    }
    return argc>2 && !strcmp(argv[2],"status37")?37:0;
}
