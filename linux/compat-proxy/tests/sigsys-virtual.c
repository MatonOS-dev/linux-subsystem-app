#define _GNU_SOURCE
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif
#define CHECK(x) do {if(!(x)){perror(#x);exit(1);}} while(0)
static volatile sig_atomic_t seen;
static void trapped(int sig,siginfo_t *info,void *opaque) {
    ucontext_t *uc=opaque;
    if(sig!=SIGSYS || info->si_code!=SYS_SECCOMP || info->si_errno!=73 ||
       (info->si_syscall!=SYS_getpid && info->si_syscall!=SYS_openat) || info->si_arch!=AUDIT_ARCH_X86_64 ||
       (uintptr_t)info->si_call_addr!=(uintptr_t)uc->uc_mcontext.gregs[REG_RIP])_exit(20);
    sigset_t mask;
    if(sigprocmask(SIG_SETMASK,NULL,&mask) || !sigismember(&mask,SIGSYS) ||
       sigismember(&uc->uc_sigmask,SIGSYS))_exit(21);
    seen++;
    uc->uc_mcontext.gregs[REG_RAX]=info->si_syscall==SYS_getpid?12345:23456;
}
static void probe(int seccomp_api) {
    struct sigaction old,action={.sa_sigaction=trapped,.sa_flags=SA_SIGINFO};
    CHECK(sigaction(SIGSYS,NULL,&old)==0);
    CHECK(old.sa_handler==SIG_DFL);
    CHECK(sigaction(SIGSYS,&action,&old)==0);
    CHECK(old.sa_handler==SIG_DFL);
    CHECK(sigaction(SIGSYS,NULL,&old)==0 && old.sa_sigaction==trapped);
    sigset_t blocked,mask;
    sigemptyset(&blocked);sigaddset(&blocked,SIGSYS);
    CHECK(sigprocmask(SIG_BLOCK,&blocked,NULL)==0);
    CHECK(sigprocmask(SIG_SETMASK,NULL,&mask)==0 && sigismember(&mask,SIGSYS)==1);
    CHECK(sigprocmask(SIG_UNBLOCK,&blocked,NULL)==0);
    CHECK(sigprocmask(SIG_SETMASK,NULL,&mask)==0 && sigismember(&mask,SIGSYS)==0);
    char alternate[16384];stack_t stack={.ss_sp=alternate,.ss_size=sizeof(alternate)},prior;
    CHECK(sigaltstack(&stack,NULL)==0);
    CHECK(sigaltstack(NULL,&prior)==0 && prior.ss_sp==alternate);
    /* Warm the libc path before filtering: it may already be rewritten. */
    int fd=openat(AT_FDCWD,"/etc/passwd",O_RDONLY);CHECK(fd>=0);CHECK(close(fd)==0);
    struct sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getpid,1,0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_openat,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP|73),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_gettid,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    struct sock_fprog filter={sizeof(code)/sizeof(code[0]),code};
    CHECK(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);
    if(seccomp_api)CHECK(syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,0,&filter)==0);
    else CHECK(prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&filter)==0);
    CHECK(syscall(SYS_getpid)==12345 && seen==1);
    CHECK(syscall(SYS_getpid)==12345 && seen==2);
    CHECK(openat(AT_FDCWD,"/etc/passwd",O_RDONLY)==23456 && seen==3);
    /* RET_ERRNO data zero skips the operation without invoking the handler. */
    errno=0;
    CHECK(syscall(SYS_gettid)==0);
    CHECK(errno==0 && seen==3);
}
int main(void) {
    pid_t child=fork();CHECK(child>=0);
    if(!child){probe(1);_exit(0);}
    int status;CHECK(waitpid(child,&status,0)==child && status==0);
    probe(0);
    puts("PASS virtual SIGSYS disposition and seccomp TRAP siginfo/ucontext in parent and fork child");
}
