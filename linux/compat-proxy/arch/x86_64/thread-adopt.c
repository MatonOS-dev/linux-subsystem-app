/* Optional resident-libc backend for the isolated v3.5 adoption launcher.
 * This file deliberately links libc; never include it in the freestanding
 * proxy build. Its narrow filter is not a security boundary or the production
 * proxy policy. Applications must reserve SIGSYS and use signal entry gates.
 * No instrumentation, stack protector, compiler TLS, or lazy PLT resolution
 * may execute with the wrong FS. All resident pthread symbols are bound at
 * startup by the build's -z now. */
#define _GNU_SOURCE
#include "../../../gpu-forward-spike/spike3/adopt.h"
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
#include <linux/futex.h>
#include <sched.h>
#include <linux/sched.h>
#include <limits.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
extern long proxy_raw(long,long,long,long,long,long,long);
extern char proxy_raw_after[];
#define R(n,a,b,c,d,e,f) proxy_raw(SYS_##n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f))
static struct adopt_thread slots[ADOPT_SLOTS];
static uintptr_t libc_lo,libc_hi;
static int force_clone;
static long identity(uintptr_t *);
static struct adopt_api api={.version=1,.current=adopt_current,.identity=identity};

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
extern char adopt_exit_return[];
static void *start(void *arg) {
    struct adopt_thread *t=arg;
    t->resident_fs=adopt_fs_read();
    int tid=R(gettid,0,0,0,0,0,0);
    atomic_store_explicit(&t->tid,tid,memory_order_release);
    atomic_store_explicit(&t->ready,1,memory_order_release);
    R(futex,&t->ready,1,INT_MAX,0,0,0);
    /* Clone inherits the application's mask, not SIGSYS handler's mask. */
    while(atomic_load_explicit(&t->ready,memory_order_acquire)!=2) R(futex,&t->ready,0,1,0,0,0);
    unsigned long mask=t->mask;
    R(rt_sigprocmask,SIG_SETMASK,&mask,0,8,0,0);
    adopt_fs_write(t->app_fs);
    adopt_run(t);
    /* Exit gate restored our own stack and FS. Resident libc owns cleanup. */
    atomic_store_explicit(&t->tid,0,memory_order_release);
    return NULL;
}
/* Non-PI Linux robust-list ABI, bounded as in the kernel. Private and shared
 * futex waiters both use shared FUTEX_WAKE for robust owner death. */
static void owner_died(uintptr_t entry,long offset,int tid) {
    if(!entry || (entry&1)) return; /* PI mutexes require kernel support. */
    _Atomic unsigned *word=(_Atomic unsigned *)(entry+offset);
    unsigned old=atomic_load(word);
    while((old&FUTEX_TID_MASK)==(unsigned)tid) {
        unsigned next=(old&FUTEX_WAITERS)|FUTEX_OWNER_DIED;
        if(atomic_compare_exchange_weak(word,&old,next)) {
            if(old&FUTEX_WAITERS) R(futex,word,FUTEX_WAKE,1,0,0,0);
            break;
        }
    }
}
static void robust_exit(struct adopt_thread *t) {
    struct robust_list_head *head=(void *)t->robust;
    if(!head) return;
    uintptr_t pending=(uintptr_t)head->list_op_pending;
    struct robust_list *p=head->list.next;
    for(unsigned count=0;p && p!=&head->list && count<2048;count++) {
        uintptr_t entry=(uintptr_t)p;
        p=((struct robust_list *)(entry&~1UL))->next;
        if(entry!=pending) owner_died(entry,head->futex_offset,atomic_load(&t->tid));
    }
    owner_died(pending,head->futex_offset,atomic_load(&t->tid));
}
static void trap(int sig,siginfo_t *info,void *opaque) {
    (void)sig;
    ucontext_t *uc=opaque;
    greg_t *g=uc->uc_mcontext.gregs;
    long n=info->si_syscall, result=-ENOSYS;
    struct adopt_thread *self=adopt_current();
    if(!self) R(exit_group,190,0,0,0,0,0);
    uintptr_t interrupted=adopt_fs_read();
    self->app_fs=interrupted;
    if(n==SYS_rt_sigaction) {
        if(g[REG_RDI]==SIGSYS && g[REG_RSI]) result=-EINVAL;
        else result=R(rt_sigaction,g[REG_RDI],g[REG_RSI],g[REG_RDX],g[REG_R10],0,0);
    } else if(n==SYS_rt_sigprocmask) {
        unsigned long old=*(unsigned long *)&uc->uc_sigmask, next=old;
        if(g[REG_R10]!=8) result=-EINVAL;
        else {
            result=0;
            if(g[REG_RSI]) {
                unsigned long mask=*(unsigned long *)g[REG_RSI];
                switch(g[REG_RDI]) {
                case SIG_SETMASK: next=mask; break;
                case SIG_BLOCK: next|=mask; break;
                case SIG_UNBLOCK: next&=~mask; break;
                default: result=-EINVAL;
                }
            }
            if(!result) {
                if(g[REG_RDX]) *(unsigned long *)g[REG_RDX]=old;
                *(unsigned long *)&uc->uc_sigmask=next & ~(1UL<<(SIGSYS-1));
            }
        }
    } else if(n==SYS_clone3 || n==SYS_clone) {
        unsigned long flags,stack,tls,ptid,ctid;
        if(n==SYS_clone3 && force_clone) {g[REG_RAX]=-ENOSYS;return;}
        if(n==SYS_clone3) {
            atomic_fetch_add(&api.clone3s,1);
            struct clone_args copy={0},*args=&copy;
            if(g[REG_RSI]<64 || (unsigned long)g[REG_RSI]>sizeof(*args)) {g[REG_RAX]=-EINVAL;return;}
            for(unsigned i=0;i<(unsigned long)g[REG_RSI];i++)
                ((unsigned char *)args)[i]=((unsigned char *)g[REG_RDI])[i];
            if(args->exit_signal || args->set_tid_size || args->cgroup) {g[REG_RAX]=-ENOSYS;return;}
            flags=args->flags; stack=args->stack+args->stack_size;
            tls=args->tls;ptid=args->parent_tid;ctid=args->child_tid;
        } else {
            atomic_fetch_add(&api.clones,1);
            flags=g[REG_RDI];stack=g[REG_RSI];tls=g[REG_R8];ptid=g[REG_RDX];ctid=g[REG_R10];
        }
        unsigned long required=CLONE_VM|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS;
        unsigned long allowed=required|CLONE_FS|CLONE_FILES|CLONE_SYSVSEM|CLONE_PARENT_SETTID|CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID;
        if((flags&required)!=required || (flags&~allowed)) result=-ENOSYS;
        else {
            struct adopt_thread *t=reserve();
            if(!t) result=-EAGAIN;
            else {
                t->app_fs=tls; t->child_tid=(flags&CLONE_CHILD_CLEARTID)?ctid:0;
                t->mask=*(unsigned long *)&uc->uc_sigmask;
                t->robust=0; atomic_store(&t->ready,0);
                for(unsigned i=0;i<23;i++) t->regs[i]=g[i];
                t->regs[REG_RSP]=stack;
                t->regs[REG_RCX]=g[REG_RIP]; t->regs[REG_R11]=g[REG_EFL];
                const unsigned char *fp=(void *)uc->uc_mcontext.fpregs;
                unsigned size=512;
                t->xfeatures=0;
                if(*(const unsigned *)(fp+464)==0x46505853) {
                    size=*(const unsigned *)(fp+480);
                    t->xfeatures=*(const uint64_t *)(fp+472);
                }
                if(size>sizeof(t->fp)) {atomic_store(&t->tid,0);g[REG_RAX]=-EOPNOTSUPP;return;}
                for(unsigned i=0;i<size;i++) t->fp[i]=fp[i];
                adopt_fs_write(self->resident_fs);
                pthread_attr_t attr; pthread_t thread;
                int error=pthread_attr_init(&attr);
                if(!error) {
                    error=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
                    if(!error) error=pthread_attr_setstacksize(&attr,128*1024);
                    if(!error) error=pthread_create(&thread,&attr,start,t);
                    pthread_attr_destroy(&attr);
                }
                adopt_fs_write(interrupted);
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
        if((unsigned long)g[REG_RSI]!=sizeof(struct robust_list_head)) result=-EINVAL;
        else {self->robust=g[REG_RDI];result=0;}
    } else if(n==SYS_get_robust_list) {
        if(g[REG_RDI] && g[REG_RDI]!=atomic_load(&self->tid)) result=-EOPNOTSUPP;
        else {*(uintptr_t *)g[REG_RSI]=self->robust;
            *(size_t *)g[REG_RDX]=sizeof(struct robust_list_head); result=0;}
    } else if(n==SYS_rseq) result=-ENOSYS;
    else if(n==SYS_set_tid_address) { self->child_tid=g[REG_RDI]; result=R(gettid,0,0,0,0,0,0); }
    else if(n==SYS_exit) {
        robust_exit(self);
        /* Consume the signal frame before publishing exit: join may unmap
         * the entire application stack as soon as the tid becomes zero. */
        adopt_fs_write(self->resident_fs);
        g[REG_RDI]=(uintptr_t)self;
        g[REG_RSP]=self->resident_sp; g[REG_RIP]=(uintptr_t)adopt_exit_return;
        g[REG_RAX]=0; return;
    } else if(n==SYS_exit_group) R(exit_group,g[REG_RDI],0,0,0,0,0);
    g[REG_RAX]=result;
}
void adopt_publish_exit(struct adopt_thread *self) {
    if(self->child_tid) {
        uintptr_t word=self->child_tid;
        atomic_store_explicit((_Atomic int *)word,0,memory_order_release);
        R(futex,word,FUTEX_WAKE,INT_MAX,0,0,0);
    }
}
static long identity(uintptr_t *p) {
    static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
    if(pthread_mutex_lock(&mutex)) __builtin_trap();
    *p=(uintptr_t)pthread_self();
    long tid=gettid();
    if(pthread_mutex_unlock(&mutex)) __builtin_trap();
    return tid;
}
_Noreturn void adopt_launch(uintptr_t entry,uintptr_t stack) {
    struct adopt_thread *t=adopt_current();
    t->regs[REG_RIP]=entry; t->regs[REG_RSP]=stack;
    t->regs[REG_EFL]=0x202;
    /* Initial linker entry does not inherit an application FP state. */
    __asm__ volatile("fxsave64 %0":"=m"(t->fp));
    t->xfeatures=0;
    adopt_fs_write(0);
    adopt_run(t);
    atomic_store_explicit(&t->tid,0,memory_order_release);
    pthread_exit(NULL);
}
int adopt_install(void) {
    _Static_assert(offsetof(struct adopt_thread,regs)==56,"assembly layout");
    _Static_assert(offsetof(struct adopt_thread,fp)==256,"FP assembly layout");
    _Static_assert(offsetof(struct adopt_thread,xfeatures)==16640,"xfeatures assembly layout");
    _Static_assert(ATOMIC_INT_LOCK_FREE==2,"signal-safe registry atomics");
    _Static_assert(ATOMIC_POINTER_LOCK_FREE==2,"signal-safe FS publication");
    struct adopt_thread *main=reserve();
    if(!main) return -1;
    main->resident_fs=adopt_fs_read(); atomic_store(&main->tid,R(gettid,0,0,0,0,0,0));
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
    force_clone=getenv("MATON_ADOPT_FORCE_CLONE")!=NULL;
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
        TRAP(rt_sigaction),TRAP(rt_sigprocmask),TRAP(clone),TRAP(clone3),TRAP(exit),TRAP(exit_group),TRAP(set_tid_address),TRAP(set_robust_list),TRAP(get_robust_list),TRAP(rseq),TRAP(fork),TRAP(vfork),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)
    };
    struct sock_fprog program={sizeof(f)/sizeof(f[0]),f};
    if(R(prctl,38,1,0,0,0,0)) return -1;
    return R(seccomp,SECCOMP_SET_MODE_FILTER,0,&program,0,0,0);
}
