#define _GNU_SOURCE
#include <signal.h>
#include <pthread.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/seccomp.h>
#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif
#include <fcntl.h>
#include <linux/prctl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
extern char __executable_start[],_etext[];
static volatile sig_atomic_t dispatches,unexpected;
static volatile sig_atomic_t unexpected_seccomp;
static long local_raw(long n,long a,long b,long c,long d,long e) {
    register long r10 __asm__("r10")=d,r8 __asm__("r8")=e;
    long result;
    __asm__ volatile("syscall":"=a"(result):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory");
    return result;
}
static void chained(int sig,siginfo_t *info,void *context) {
    (void)sig;
    if(info->si_code==SYS_SECCOMP) {unexpected_seccomp++;return;}
    if(info->si_code!=2) {unexpected++;return;}
    dispatches++;
    local_raw(SYS_prctl,PR_SET_SYSCALL_USER_DISPATCH,PR_SYS_DISPATCH_OFF,0,0,0);
    ((ucontext_t *)context)->uc_mcontext.gregs[REG_RAX]=777;
    /* A chained handler must not disable the private stack or block SIGSYS
     * by editing the sigreturn context. */
    ((ucontext_t *)context)->uc_stack.ss_flags=SS_DISABLE;
    sigaddset(&((ucontext_t *)context)->uc_sigmask,SIGSYS);
}
#define CHECK(c) do { if(!(c)) {fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#c,errno);return 1;} } while(0)
int main(void) {
    struct sigaction act={0},old={0};act.sa_sigaction=chained;act.sa_flags=SA_SIGINFO|SA_RESETHAND|SA_ONSTACK;
    sigfillset(&act.sa_mask);CHECK(sigaction(SIGSYS,&act,NULL)==0);
    /* libc is a resident IP exemption. Its SIGSYS registration must still go
     * through the proxy so subsequent trapped path calls remain translated. */
    long fd=local_raw(SYS_openat,AT_FDCWD,(long)"/etc/passwd",O_RDONLY,0,0);
    CHECK(fd>=0);CHECK(!unexpected_seccomp);
    CHECK(local_raw(SYS_close,fd,0,0,0,0)==0);CHECK(!unexpected_seccomp);
    sigset_t mask,got;sigfillset(&mask);
    CHECK(pthread_sigmask(SIG_SETMASK,&mask,NULL)==0);
    CHECK(pthread_sigmask(SIG_SETMASK,NULL,&got)==0);CHECK(!sigismember(&got,SIGSYS));CHECK(sigismember(&got,SIGCHLD));
    errno=0;CHECK(syscall(SYS_getpgrp)==getpgid(0));CHECK(!unexpected);
    CHECK(sigprocmask(SIG_BLOCK,&mask,NULL)==0);
    errno=0;CHECK(syscall(SYS_getpgrp)==getpgid(0));
    void *alt=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(alt!=MAP_FAILED);
    stack_t ss={.ss_sp=alt,.ss_size=65536};CHECK(sigaltstack(&ss,NULL)==0);
    ss.ss_flags=SS_DISABLE;CHECK(sigaltstack(&ss,NULL)==0);CHECK(munmap(alt,65536)==0);
    errno=0;CHECK(syscall(SYS_getpgrp)==getpgid(0));
    /* Actual SYS_USER_DISPATCH from an executable syscall stub outside the allow interval. */
    unsigned char code[]={0xb8,39,0,0,0,0x0f,0x05,0xc3};
    void *stub=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(stub!=MAP_FAILED);
    memcpy(stub,code,sizeof(code));CHECK(mprotect(stub,4096,PROT_READ|PROT_EXEC)==0);
    volatile unsigned char selector=SYSCALL_DISPATCH_FILTER_BLOCK;
    long r=local_raw(SYS_prctl,PR_SET_SYSCALL_USER_DISPATCH,PR_SYS_DISPATCH_ON,
                    (long)__executable_start,0x600004000000UL-(long)__executable_start,(long)&selector);
    if(r==-EINVAL || r==-ENOSYS) {puts("SKIP SYS_USER_DISPATCH unavailable");}
    else {CHECK(r==0);CHECK(((long (*)(void))stub)()==777);CHECK(dispatches==1);CHECK(!unexpected);
        CHECK(sigaction(SIGSYS,NULL,&old)==0);CHECK(old.sa_handler==SIG_DFL);}
    errno=0;CHECK(syscall(SYS_getpgrp)==getpgid(0));
    puts("PASS SIGSYS: pthread/sigprocmask, altstack disable, SA_RESETHAND, seccomp and user-dispatch demux");
    return 0;
}
