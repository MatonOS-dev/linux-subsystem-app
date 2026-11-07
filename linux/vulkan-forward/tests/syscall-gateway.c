/* Same source on both ABIs: inherited payload policy must not trap the
 * forwarder's internal calls; count signals rather than timing alone. */
#define _GNU_SOURCE
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <signal.h>
#include <stdint.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "../runtime.h"
#include "backend.h"
#include "../../gpu-bridge-common/bootstrap.h"
extern long mv_test_gateway(long,long,long,long,long,long,long);
extern char mv_test_gateway_after[];
static volatile sig_atomic_t traps;
#ifdef __ANDROID__
/* NDK headers, host glibc linker: same assert behaviour, different symbol. */
void __assert2(const char *file,int line,const char *fn,const char *expression) {
    (void)file;(void)line;(void)fn;(void)expression;_exit(90);
}
#endif
#ifdef __x86_64__
#define NR_PID 39
#define NR_MAP 9
#define RESULT REG_RAX
#else
#define NR_PID 20
#define NR_MAP 192
#define RESULT REG_EAX
#endif
#ifdef __x86_64__
static void trap(int sig,siginfo_t *info,void *ctx) {
    assert(sig==SIGSYS && info->si_syscall==NR_PID);traps++;
    ((ucontext_t *)ctx)->uc_mcontext.gregs[RESULT]=mv_test_gateway(NR_PID,0,0,0,0,0,0);
}
#endif
static double now(void) {struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec+t.tv_nsec/1e9;}
int main(void) {
    struct maton_gpu_bootstrap *b=mmap(NULL,4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(b!=MAP_FAILED);
    b->magic=MATON_GPU_BOOTSTRAP_MAGIC;
    snprintf(b->syscall,sizeof(b->syscall),"%lx",(unsigned long)mv_test_gateway);
    assert(!prctl(PR_SET_VMA,PR_SET_VMA_ANON_NAME,b,4096,MATON_GPU_BOOTSTRAP_NAME));
    assert(!mprotect(b,4096,PROT_READ));
    assert(mv_env("MATON_GPU_BRIDGE")); /* real named mapping discovery */
#ifdef __x86_64__
    struct sigaction action={.sa_sigaction=trap,.sa_flags=SA_SIGINFO};assert(!sigaction(SIGSYS,&action,NULL));
#endif
    uint64_t pc=(uintptr_t)mv_test_gateway_after;
    struct sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)+4),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,(uint32_t)(pc>>32),0,3),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,(uint32_t)pc,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,NR_PID,0,1),
#ifdef __x86_64__
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP),
#else
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|13),
#endif
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,NR_MAP,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|13),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program={sizeof(code)/sizeof(code[0]),code};
    assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program));
    long pid=mv_test_gateway(NR_PID,0,0,0,0,0,0);
    const unsigned iterations=100000;double start=now();
    for(unsigned i=0;i<iterations;i++)assert(mv_native_sys(NR_PID,0,0,0,0,0,0)==
#ifdef __x86_64__
        pid
#else
        -13
#endif
    );
    double before=(now()-start)*1e9/iterations;
#ifdef __x86_64__
    assert(traps==(sig_atomic_t)iterations);
#endif
    traps=0;start=now();
    for(unsigned i=0;i<iterations;i++)assert(mv_sys(NR_PID,0,0,0,0,0,0)==pid);
    double after=(now()-start)*1e9/iterations;assert(!traps);
    assert(mv_native_sys(NR_MAP,0,4096,3,0x22,-1,0)==-13);
    void *memory=mv_alloc(4096);assert(memory);memset(memory,42,4096);mv_free(memory);
    assert(mv_read(-1,NULL,0)==-9); /* raw errno, not libc -1/errno */
    pid_t child=fork();assert(child>=0);
    if(!child){assert(mv_sys(NR_PID,0,0,0,0,0,0)>0 && !traps);_exit(0);}
    int status;assert(waitpid(child,&status,0)==child && !status);
#ifdef __x86_64__
    printf("PASS %zu-bit forwarder gateway: mmap under denied payload policy; fork; raw errno; %.0f -> %.0f ns/call; %u -> 0 SIGSYS per %u calls\n",sizeof(void *)*8,before,after,iterations,iterations);
#else
    printf("PASS 32-bit forwarder gateway: denied mmap/getpid bypass; fork; raw errno; %.0f ns/direct call (errno control %.0f ns)\n",after,before);
#endif
    fflush(NULL);
    return 0;
}
