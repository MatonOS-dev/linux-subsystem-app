#define _GNU_SOURCE
/* Kernel-only signal round trip. Run natively, outside the compat proxy. */
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
static volatile unsigned long hits;
static long parent;
static void trap(int sig,siginfo_t *info,void *context) {
    (void)sig;
    if(info->si_code!=1 /* SYS_SECCOMP */ || info->si_syscall!=SYS_getppid)_exit(3);
    ((ucontext_t *)context)->uc_mcontext.gregs[REG_RAX]=parent;
    hits++;
}
static double now(void) {struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))exit(2);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char **argv) {
    long loops=argc>1?strtol(argv[1],NULL,10):30000;
    if(loops<=0 || argc>2)return 2;
    parent=getppid();
    struct sigaction sa={.sa_sigaction=trap,.sa_flags=SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    if(sigaction(SIGSYS,&sa,NULL))return 2;
    struct sock_filter insns[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getppid,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog={sizeof(insns)/sizeof(insns[0]),insns};
    for(int phase=0;phase<2;phase++) {
        if(phase && (prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0) || syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,0,&prog))) {perror("seccomp");return 2;}
        for(int round=0;round<3;round++) {
            double start=now();
            for(long i=0;i<loops;i++)if(syscall(SYS_getppid)!=parent)return 3;
            printf("%s %.6f us/op\n",phase?"kernel-sigsys-return":"native-getppid",(now()-start)*1e6/loops);
        }
    }
    if(hits!=(unsigned long)(3*loops))return 3;
    return 0;
}
