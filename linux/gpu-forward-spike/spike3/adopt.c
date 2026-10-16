#define _GNU_SOURCE
#include "adopt.h"
#include <stddef.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <sched.h>
#include <linux/sched.h>
#include <limits.h>
#include <dlfcn.h>
#include <unistd.h>
extern long proxy_raw(long,long,long,long,long,long,long);
extern char proxy_raw_after[];
#define R(n,a,b,c,d,e,f) proxy_raw(SYS_##n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f))
static struct adopt_thread slots[ADOPT_SLOTS];
static uintptr_t libc_lo,libc_hi;
static uintptr_t fs_read(void) { uintptr_t fs; R(arch_prctl,0x1003,&fs,0,0,0,0); return fs; }
static void fs_write(uintptr_t fs) { R(arch_prctl,0x1002,fs,0,0,0,0); }
struct adopt_thread *adopt_current(void) {
    int tid=R(gettid,0,0,0,0,0,0);
    for(unsigned i=0;i<ADOPT_SLOTS;i++)
        if(atomic_load_explicit(&slots[i].tid,memory_order_acquire)==tid) return &slots[i];
    return NULL;
}
static struct adopt_thread *reserve(void) {
    for(unsigned i=0;i<ADOPT_SLOTS;i++) { int zero=0;
        if(atomic_compare_exchange_strong(&slots[i].tid,&zero,-1)) return &slots[i];
    }
    return NULL;
}
extern void adopt_run(struct adopt_thread *);
extern char adopt_return[];
static void *start(void *arg) {
    struct adopt_thread *t=arg;
    t->resident_fs=fs_read();
    int tid=R(gettid,0,0,0,0,0,0);
    atomic_store_explicit(&t->tid,tid,memory_order_release);
    atomic_store_explicit(&t->ready,1,memory_order_release);
    R(futex,&t->ready,1,INT_MAX,0,0,0);
    /* Clone inherits the application's mask, not SIGSYS handler's mask. */
    while(atomic_load_explicit(&t->ready,memory_order_acquire)!=2) R(futex,&t->ready,0,1,0,0,0);
    unsigned long mask=t->mask;
    R(rt_sigprocmask,SIG_SETMASK,&mask,0,8,0,0);
    fs_write(t->app_fs);
    adopt_run(t);
    /* Exit gate restored our own stack and FS. Resident libc owns cleanup. */
    atomic_store_explicit(&t->tid,0,memory_order_release);
    return NULL;
}
static void trap(int sig,siginfo_t *info,void *opaque) {
    (void)sig;
    ucontext_t *uc=opaque;
    greg_t *g=uc->uc_mcontext.gregs;
    long n=info->si_syscall, result=-38;
    struct adopt_thread *self=adopt_current();
    if(!self) R(exit_group,190,0,0,0,0,0);
    uintptr_t interrupted=fs_read();
    self->app_fs=interrupted;
    if(n==SYS_rt_sigprocmask) {
        unsigned long old=*(unsigned long *)&uc->uc_sigmask, next=old;
        if(g[REG_R10]!=8) result=-22;
        else {
            result=0;
            if(g[REG_RSI]) {
                unsigned long mask=*(unsigned long *)g[REG_RSI];
                switch(g[REG_RDI]) {
                case SIG_SETMASK: next=mask; break;
                case SIG_BLOCK: next|=mask; break;
                case SIG_UNBLOCK: next&=~mask; break;
                default: result=-22;
                }
            }
            if(!result) {
                if(g[REG_RDX]) *(unsigned long *)g[REG_RDX]=old;
                *(unsigned long *)&uc->uc_sigmask=next & ~(1UL<<(SIGSYS-1));
            }
        }
    } else if(n==SYS_clone3 || n==SYS_clone) {
        unsigned long flags,stack,tls,ptid,ctid;
        if(n==SYS_clone3) {
            struct clone_args *args=(void *)g[REG_RDI];
            if(g[REG_RSI]<64 || g[REG_RSI]>sizeof(*args)) {g[REG_RAX]=-22;return;}
            if(args->exit_signal || args->set_tid_size || args->cgroup || args->pidfd) {g[REG_RAX]=-38;return;}
            flags=args->flags; stack=args->stack+args->stack_size;
            tls=args->tls;ptid=args->parent_tid;ctid=args->child_tid;
        } else {
            flags=g[REG_RDI];stack=g[REG_RSI];tls=g[REG_R8];ptid=g[REG_RDX];ctid=g[REG_R10];
        }
        unsigned long required=CLONE_VM|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS;
        unsigned long allowed=required|CLONE_FS|CLONE_FILES|CLONE_SYSVSEM|CLONE_PARENT_SETTID|CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID;
        if((flags&required)!=required || (flags&~allowed)) result=-38;
        else {
            struct adopt_thread *t=reserve();
            if(!t) result=-11;
            else {
                t->app_fs=tls; t->child_tid=(flags&CLONE_CHILD_CLEARTID)?ctid:0;
                t->mask=*(unsigned long *)&uc->uc_sigmask;
                t->robust=0; atomic_store(&t->ready,0);
                for(unsigned i=0;i<23;i++) t->regs[i]=g[i];
                t->regs[REG_RSP]=stack;
                t->regs[REG_RCX]=g[REG_RIP]; t->regs[REG_R11]=g[REG_EFL];
                fs_write(self->resident_fs);
                pthread_attr_t attr; pthread_t thread;
                pthread_attr_init(&attr);
                pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
                pthread_attr_setstacksize(&attr,128*1024);
                int error=pthread_create(&thread,&attr,start,t);
                pthread_attr_destroy(&attr);
                fs_write(interrupted);
                if(error) {atomic_store(&t->tid,0);result=-error;}
                else {
                    while(!atomic_load_explicit(&t->ready,memory_order_acquire)) R(futex,&t->ready,0,0,0,0,0);
                    result=atomic_load(&t->tid);
                    if(flags&CLONE_PARENT_SETTID) *(int *)ptid=result;
                    if(flags&CLONE_CHILD_SETTID) *(int *)ctid=result;
                    atomic_store_explicit(&t->ready,2,memory_order_release);
                    R(futex,&t->ready,1,INT_MAX,0,0,0);
                }
            }
        }
    } else if(n==SYS_set_robust_list) {
        /* Keep resident kernel registration. Glibc robust mutexes unsupported. */
        result=-38;
    } else if(n==SYS_rseq) result=-38;
    else if(n==SYS_set_tid_address) { self->child_tid=g[REG_RDI]; result=R(gettid,0,0,0,0,0,0); }
    else if(n==SYS_exit) {
        if(self==&slots[0]) R(exit_group,g[REG_RDI],0,0,0,0,0);
        if(self->child_tid) { *(int *)self->child_tid=0; R(futex,self->child_tid,1,INT_MAX,0,0,0); }
        fs_write(self->resident_fs);
        g[REG_RSP]=self->resident_sp; g[REG_RIP]=(uintptr_t)adopt_return;
        g[REG_RAX]=0; return;
    } else if(n==SYS_exit_group) R(exit_group,g[REG_RDI],0,0,0,0,0);
    g[REG_RAX]=result;
}
static long identity(uintptr_t *p) { *p=(uintptr_t)pthread_self(); return gettid(); }
int adopt_install(void) {
    _Static_assert(offsetof(struct adopt_thread,regs)==56,"assembly layout");
    struct adopt_thread *main=reserve();
    main->resident_fs=fs_read(); atomic_store(&main->tid,R(gettid,0,0,0,0,0,0));
    /* Exempt resident libc syscall instructions, never app libc. */
    Dl_info di;
    if(!dladdr((void *)pthread_create,&di)) return -1;
    FILE *maps=fopen("/proc/self/maps","r"); if(!maps) return -1;
    char line[1024]; unsigned long lo,hi; char perms[5];
    while(fgets(line,sizeof(line),maps)) {
        if(sscanf(line,"%lx-%lx %4s",&lo,&hi,perms)==3 && perms[2]=='x' &&
           lo>(uintptr_t)di.dli_fbase && lo<(uintptr_t)di.dli_fbase+0x1000000) {
            /* Select mapping containing pthread_create itself. */
            if((uintptr_t)pthread_create>=lo && (uintptr_t)pthread_create<hi) {libc_lo=lo;libc_hi=hi;}
        }
    }
    fclose(maps);
    if(!libc_lo || libc_lo>>32 != (libc_hi-1)>>32) return -1;
    static struct adopt_api api={1,adopt_current,identity};
    char value[32]; snprintf(value,sizeof(value),"%lx",(unsigned long)&api);
    setenv("MATON_THREAD_ADOPT",value,1);
    struct sigaction sa={0}; sa.sa_sigaction=trap; sa.sa_flags=SA_SIGINFO;
    sigemptyset(&sa.sa_mask); if(sigaction(SIGSYS,&sa,NULL)) return -1;
    struct sock_filter f[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,4),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,12),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,(uintptr_t)proxy_raw_after>>32,0,3),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,8),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,(uintptr_t)proxy_raw_after,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,12),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,libc_lo>>32,0,4),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,8),
        BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,libc_lo,0,2),
        BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,libc_hi,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),
#define TRAP(n) BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_##n,0,1), BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP)
        TRAP(rt_sigprocmask),TRAP(clone),TRAP(clone3),TRAP(exit),TRAP(exit_group),TRAP(set_tid_address),TRAP(set_robust_list),TRAP(rseq),TRAP(fork),TRAP(vfork),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)
    };
    struct sock_fprog program={sizeof(f)/sizeof(f[0]),f};
    if(R(prctl,38,1,0,0,0,0)) return -1;
    return R(seccomp,SECCOMP_SET_MODE_FILTER,0,&program,0,0,0);
}
