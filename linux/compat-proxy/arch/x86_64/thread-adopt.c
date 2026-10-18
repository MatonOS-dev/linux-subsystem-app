/* Optional resident-libc backend for the isolated v3.5 adoption launcher.
 * This file deliberately links libc; never include it in the freestanding
 * proxy build. Seccomp routes the Linux ABI calls that need resident
 * bookkeeping; reserve SIGSYS and use signal gates.
 * No instrumentation, stack protector, compiler TLS, or lazy PLT resolution
 * may execute with the wrong FS. All resident pthread symbols are bound at
 * startup by the build's -z now. */
#define _GNU_SOURCE
#include "../../../gpu-forward-spike/spike3/adopt.h"
#include "adopt.h"
#include "../../adopt-map.h"
#include <stddef.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <sys/prctl.h>
#include <linux/seccomp.h>
#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif
#include <linux/futex.h>
#include <sched.h>
#include <linux/sched.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <sys/uio.h>
extern long proxy_raw(long,long,long,long,long,long,long);
extern char proxy_clone_entry[],adopt_vfork_entry[];
extern void proxy_restore(void);
#define R(n,a,b,c,d,e,f) proxy_raw(SYS_##n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f))
/* Chunk size controls allocation granularity, not registry capacity. Published
 * chunks are never unmapped: interrupted lock-free readers may retain them. */
enum { CHUNK_THREADS=32 };
struct thread_chunk {
    _Atomic(struct thread_chunk *) next;
    struct adopt_thread entries[CHUNK_THREADS];
};
static _Atomic(struct thread_chunk *) chunks;
/* Two independent glibc allocators cannot share sbrk: one can shrink the
 * other's live heap. Freeze only the app's program break; its malloc uses
 * mmap fallback without an imposed memory cap. Resident brk remains native. */
static uintptr_t app_brk;
static int force_clone;
static size_t live(void);
static size_t signal_stack_size;
static long identity(uintptr_t *);
static size_t live(void);
static pthread_mutex_t identity_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct adopt_api api={.version=2,.current=adopt_current,.lookup=adopt_map_lookup,
                             .identity=identity,.live=live};
static int fs_fast;
static struct {
    uintptr_t handler;
    unsigned long flags;
    void (*restorer)(void);
    unsigned long mask;
} prior_sigsys;

uintptr_t adopt_api_address(void) { return (uintptr_t)&api; }

/* FSGSBASE is a CPU feature; the kernel advertises it in AT_HWCAP2 bit 1 and
 * the launcher/ICD publish the same bit. rdfsbase is then a register read with
 * no trap; without it we fall back to arch_prctl(ARCH_GET_FS) through the
 * always-allowed raw gateway. */
static uintptr_t backend_fs_read(void) {
    uintptr_t fs=0;
    if(fs_fast) { __asm__ volatile("rdfsbase %0":"=r"(fs)::"memory"); return fs; }
    if(R(arch_prctl,0x1003,&fs,0,0,0,0)) __builtin_trap();
    return fs;
}
static void backend_fs_write(uintptr_t fs) {
    if(R(arch_prctl,0x1002,fs,0,0,0,0)) __builtin_trap();
}
/* Zero-initialised table memory for the FS map, from the raw gateway; never
 * the resident or application malloc, which the SIGSYS handler cannot use. */
static void *map_memory(size_t n) {
    long p=R(mmap,0,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    return (unsigned long)p>=(unsigned long)-4095?0:(void *)p;
}
static long signal_stack_install(struct adopt_thread *t) {
    if(R(sigaltstack,0,&t->prior_signal_stack,0,0,0,0)) return -1;
    long p=R(mmap,0,signal_stack_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if((unsigned long)p>=(unsigned long)-4095) return p;
    t->signal_stack=(void *)p;
    stack_t ss={.ss_sp=(void *)p,.ss_size=signal_stack_size};
    long error=R(sigaltstack,&ss,0,0,0,0,0);
    if(error) {R(munmap,p,signal_stack_size,0,0,0,0);t->signal_stack=NULL;}
    return error;
}
static void signal_stack_release(struct adopt_thread *t) {
    if(R(sigaltstack,&t->prior_signal_stack,0,0,0,0,0)) __builtin_trap();
    R(munmap,t->signal_stack,signal_stack_size,0,0,0,0);t->signal_stack=NULL;
}
__attribute__((target("general-regs-only")))
struct adopt_thread *adopt_current(void) {
    /* Fast path: key on the current FS base. FS base 0 (the pre-TLS startup
     * window) is never inserted, so a miss falls back to the tid scan and
     * self-heals the map once the thread has a real TLS block. */
    uintptr_t fs=backend_fs_read();
    struct adopt_thread *found=adopt_map_lookup(fs);
    if(found) return found;
    int tid=R(gettid,0,0,0,0,0,0);
    for(struct thread_chunk *c=atomic_load_explicit(&chunks,memory_order_acquire);c;
        c=atomic_load_explicit(&c->next,memory_order_acquire))
        for(unsigned i=0;i<CHUNK_THREADS;i++)
            if(atomic_load_explicit(&c->entries[i].tid,memory_order_acquire)==tid) {
                adopt_map_bind(fs,&c->entries[i]);
                return &c->entries[i];
            }
    return NULL;
}
static struct adopt_thread *reserve(void) {
    _Atomic(struct thread_chunk *) *link=&chunks;
    for(;;) {
        struct thread_chunk *c=atomic_load_explicit(link,memory_order_acquire);
        if(!c) {
            /* Fixed-size multiplication is checked at compile time; no growing
             * capacity arithmetic, tid-indexed allocations or size truncation. */
            _Static_assert(CHUNK_THREADS<=SIZE_MAX/sizeof(struct adopt_thread)-1,
                           "registry chunk size overflow");
            long memory=R(mmap,0,sizeof(*c),PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if((unsigned long)memory>=(unsigned long)-4095) return NULL;
            struct thread_chunk *fresh=(void *)memory;
            if(atomic_compare_exchange_strong_explicit(link,&c,fresh,
                   memory_order_release,memory_order_acquire)) c=fresh;
            else R(munmap,fresh,sizeof(*fresh),0,0,0,0);
        }
        for(unsigned i=0;i<CHUNK_THREADS;i++) {
            int zero=0;
            if(atomic_compare_exchange_strong(&c->entries[i].tid,&zero,-1)) {
                c->entries[i].resident_sp=0;
                atomic_store(&c->entries[i].exec_kicked,0);
                return &c->entries[i];
            }
        }
        link=&c->next;
    }
}
/* Parent-only continuation recovery. The vfork child never reads records. */
__attribute__((target("general-regs-only")))
uintptr_t adopt_vfork_parent(void) {
    struct adopt_thread *t=adopt_current();
    if(!t) __builtin_trap();
    return t->vfork_resume;
}
static long clone_reject(const char *line,long error) {
    size_t n=0;while(line[n]) n++;
    R(write,2,line,n,0,0,0);
    return -error;
}
#define REJECT(reason,error) clone_reject("maton adopt: clone rejected: " reason " (" #error ")\n",error)
static size_t live(void) {
    size_t count=0;
    for(struct thread_chunk *c=atomic_load(&chunks);c;c=atomic_load(&c->next))
        for(unsigned i=0;i<CHUNK_THREADS;i++)
            if(atomic_load(&c->entries[i].tid)!=0) {
                if(count==SIZE_MAX) __builtin_trap();
                count++;
            }
    return count;
}
static void fork_rebase(struct adopt_thread *self) {
    /* Retain mappings (including the caller's parked resident stack) but
     * discard every vanished identity/reservation. No parent memory changes. */
    for(struct thread_chunk *c=atomic_load(&chunks);c;c=atomic_load(&c->next))
        for(unsigned i=0;i<CHUNK_THREADS;i++)
            if(&c->entries[i]!=self) {
                if(c->entries[i].signal_stack) {
                    R(munmap,c->entries[i].signal_stack,signal_stack_size,0,0,0,0);
                    c->entries[i].signal_stack=NULL;
                }
                atomic_store(&c->entries[i].tid,0);
            }
    adopt_map_reset();
    self->resident_fs=backend_fs_read();
    adopt_map_bind(self->resident_fs,self);
    adopt_map_bind(atomic_load(&self->app_fs),self);
    atomic_store(&self->tid,R(gettid,0,0,0,0,0,0));
}
static long resident_fork(struct adopt_thread *self,unsigned long flags,
                          uintptr_t ptid,uintptr_t ctid) {
    uintptr_t app_fs=self->app_fs;
    backend_fs_write(self->resident_fs);
    pid_t pid=fork();
    int error=errno;
    if(!pid) {
        fork_rebase(self);
        self->child_tid=(flags&CLONE_CHILD_CLEARTID)?ctid:0;
        if(flags&CLONE_CHILD_SETTID) *(int *)ctid=atomic_load(&self->tid);
    } else if(pid>0 && (flags&CLONE_PARENT_SETTID)) *(int *)ptid=pid;
    backend_fs_write(app_fs);
    return pid<0?-error:pid;
}
static void resident_prepare(void) {
    if(pthread_mutex_lock(&identity_mutex)) __builtin_trap();
    atomic_fetch_add(&api.fork_prepares,1);
}
static void resident_parent(void) {
    atomic_fetch_add(&api.fork_parents,1);
    if(pthread_mutex_unlock(&identity_mutex)) __builtin_trap();
}
static void resident_child(void) {
    if(gettid()!=R(gettid,0,0,0,0,0,0) || getpid()!=R(getpid,0,0,0,0,0,0))
        __builtin_trap();
    atomic_fetch_add(&api.fork_children,1);
    if(pthread_mutex_unlock(&identity_mutex)) __builtin_trap();
}
extern void adopt_run(struct adopt_thread *);
extern char adopt_exit_return[];
static void *start(void *arg) {
    struct adopt_thread *t=arg;
    t->resident_fs=backend_fs_read();
    adopt_map_bind(t->resident_fs,t);
    if(signal_stack_install(t)) R(exit_group,193,0,0,0,0,0);
    int tid=R(gettid,0,0,0,0,0,0);
    atomic_store_explicit(&t->tid,tid,memory_order_release);
    atomic_store_explicit(&t->ready,1,memory_order_release);
    R(futex,&t->ready,1,INT_MAX,0,0,0);
    /* Clone inherits the application's mask, not SIGSYS handler's mask. */
    while(atomic_load_explicit(&t->ready,memory_order_acquire)!=2) R(futex,&t->ready,0,1,0,0,0);
    unsigned long mask=t->mask;
    R(rt_sigprocmask,SIG_SETMASK,&mask,0,8,0,0);
    backend_fs_write(t->app_fs);
    adopt_run(t);
    /* Exit gate restored our own stack and FS. Resident libc owns cleanup. */
    signal_stack_release(t);
    adopt_map_unbind(t->resident_fs,t);
    adopt_map_unbind(atomic_load(&t->app_fs),t);
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
static int read_user(void *out,const void *in,size_t size) {
    struct iovec local={out,size},remote={(void *)in,size};
    return R(process_vm_readv,R(getpid,0,0,0,0,0,0),&local,1,&remote,1,0)==(long)size;
}
static void robust_exit(struct adopt_thread *t) {
    struct robust_list_head *head=(void *)t->robust,copy;
    if(!head || !read_user(&copy,head,sizeof(copy))) return;
    uintptr_t pending=(uintptr_t)copy.list_op_pending;
    struct robust_list *p=copy.list.next;
    for(unsigned count=0;p && p!=&head->list && count<2048;count++) {
        uintptr_t entry=(uintptr_t)p;
        if(!read_user(&p,(void *)(entry&~1UL),sizeof(p))) return;
        if(entry!=pending) owner_died(entry,copy.futex_offset,atomic_load(&t->tid));
    }
    owner_died(pending,copy.futex_offset,atomic_load(&t->tid));
}
static void trap(int sig,siginfo_t *info,void *opaque) {
    if(info->si_code!=SYS_SECCOMP) {
        if(prior_sigsys.handler>1) {
            if(prior_sigsys.flags&SA_SIGINFO)
                ((void (*)(int,siginfo_t *,void *))prior_sigsys.handler)(sig,info,opaque);
            else ((void (*)(int))prior_sigsys.handler)(sig);
            return;
        }
        R(exit_group,191,0,0,0,0,0);
    }
    long n=info->si_syscall;
    /* Only Linux process/thread ABI calls need adoption. Other trapped calls
     * belong to the compatibility proxy's seccomp handler. */
    if(n!=SYS_brk && n!=SYS_munmap && n!=SYS_rt_sigaction && n!=SYS_rt_sigprocmask &&
       n!=SYS_fork && n!=SYS_vfork && n!=SYS_clone && n!=SYS_clone3 &&
       n!=SYS_set_robust_list && n!=SYS_get_robust_list && n!=SYS_rseq &&
       n!=SYS_set_tid_address && n!=SYS_exit && n!=SYS_exit_group && n!=SYS_arch_prctl) {
        if(prior_sigsys.handler>1 && (prior_sigsys.flags&SA_SIGINFO))
            ((void (*)(int,siginfo_t *,void *))prior_sigsys.handler)(sig,info,opaque);
        else R(exit_group,191,0,0,0,0,0);
        return;
    }
    ucontext_t *uc=opaque;
    greg_t *g=uc->uc_mcontext.gregs;
    long result=-ENOSYS;
    struct adopt_thread *self=adopt_current();
    if(!self) { R(exit_group,190,0,0,0,0,0); __builtin_unreachable(); }
    uintptr_t interrupted=backend_fs_read();
    self->app_fs=interrupted;
    if(n==SYS_munmap && (uintptr_t)g[REG_RDI]<=(uintptr_t)g[REG_RSP] &&
              (uintptr_t)g[REG_RSP]-(uintptr_t)g[REG_RDI]<(uintptr_t)g[REG_RSI]) {
        /* The kernel permits unmap-self followed by exit without any stack
         * access. SIGSYS runs on our own altstack; consume that live frame
         * after unmapping, rather than retrying on the disappearing stack. */
        result=R(munmap,g[REG_RDI],g[REG_RSI],0,0,0,0);
    }
    else if(n==SYS_brk) result=app_brk;
    else if(n==SYS_rt_sigaction) {
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
    }
    else if(n==SYS_fork) result=resident_fork(self,0,0,0);
    else if(n==SYS_vfork) {g[REG_R11]=g[REG_RSP];goto vfork_retry;}
    else if(n==SYS_clone3 || n==SYS_clone) {
        unsigned long flags,stack,tls,ptid,ctid;
        unsigned long exit_signal;
        unsigned long child_stack;
        if(n==SYS_clone3 && force_clone) {result=-ENOSYS;goto finish;}
        if(n==SYS_clone3) {
            atomic_fetch_add(&api.clone3s,1);
            struct clone_args copy={0},*args=&copy;
            if(g[REG_RSI]<64 || (unsigned long)g[REG_RSI]>sizeof(*args)) {result=REJECT("clone3 size",EINVAL);goto finish;}
            for(unsigned i=0;i<(unsigned long)g[REG_RSI];i++)
                ((unsigned char *)args)[i]=((unsigned char *)g[REG_RDI])[i];
            if(args->set_tid || args->set_tid_size || args->cgroup) {result=REJECT("clone3 fields",ENOSYS);goto finish;}
            flags=args->flags; exit_signal=args->exit_signal;
            if(args->stack_size>UINTPTR_MAX-args->stack) {result=REJECT("stack overflow",EINVAL);goto finish;}
            stack=args->stack+args->stack_size;
            if((args->stack==0)!=(args->stack_size==0)) {result=REJECT("inconsistent stack base/size",EINVAL);goto finish;}
            tls=args->tls;ptid=args->parent_tid;ctid=args->child_tid;
        } else {
            atomic_fetch_add(&api.clones,1);
            exit_signal=g[REG_RDI]&CSIGNAL;
            flags=g[REG_RDI]&~CSIGNAL;stack=g[REG_RSI];tls=g[REG_R8];ptid=g[REG_RDX];ctid=g[REG_R10];
        }
        child_stack=stack?stack:(unsigned long)g[REG_RSP];
        if((flags&(CLONE_VM|CLONE_VFORK))==(CLONE_VM|CLONE_VFORK) && !(flags&CLONE_THREAD)) {
            unsigned long allowed=CLONE_VM|CLONE_VFORK|CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|CLONE_PARENT_SETTID;
            if(n==SYS_clone3) allowed|=CLONE_CLEAR_SIGHAND;
            if((flags&~allowed) || exit_signal!=SIGCHLD) {result=REJECT("vfork flags/signal",EINVAL);goto finish;}
            g[REG_R11]=child_stack;
            goto vfork_retry;
        }
        if(!(flags&(CLONE_VM|CLONE_THREAD)) && exit_signal==SIGCHLD &&
           !(flags&~(unsigned long)(CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|CLONE_PARENT_SETTID))) {
            result=resident_fork(self,flags,ptid,ctid);
            goto finish;
        }
        if(exit_signal) {result=REJECT("unsupported process clone/signal",EINVAL);goto finish;}
        unsigned long required=CLONE_VM|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS|CLONE_FS|CLONE_FILES|CLONE_SYSVSEM;
        unsigned long allowed=required|CLONE_DETACHED|CLONE_FS|CLONE_FILES|CLONE_SYSVSEM|CLONE_PARENT_SETTID|CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID;
        if((flags&required)!=required || (flags&~allowed)) result=REJECT("thread flags",ENOSYS);
        else {
            struct adopt_thread *t=reserve();
            if(!t) result=-EAGAIN;
            else {
                t->app_fs=tls; t->child_tid=(flags&CLONE_CHILD_CLEARTID)?ctid:0;
                adopt_map_bind(tls,t);
                t->mask=*(unsigned long *)&uc->uc_sigmask;
                t->robust=0; atomic_store(&t->ready,0);
                for(unsigned i=0;i<23;i++) t->regs[i]=g[i];
                t->regs[REG_RSP]=stack?stack:(unsigned long)g[REG_RSP];
                t->regs[REG_RCX]=g[REG_RIP]; t->regs[REG_R11]=g[REG_EFL];
                const unsigned char *fp=(void *)uc->uc_mcontext.fpregs;
                unsigned size=512;
                t->xfeatures=0;
                if(*(const unsigned *)(fp+464)==0x46505853) {
                    size=*(const unsigned *)(fp+480);
                    t->xfeatures=*(const uint64_t *)(fp+472);
                }
                if(size>sizeof(t->fp)) {atomic_store(&t->tid,0);adopt_map_unbind(tls,t);result=-EOPNOTSUPP;goto finish;}
                for(unsigned i=0;i<size;i++) t->fp[i]=fp[i];
                backend_fs_write(self->resident_fs);
                pthread_attr_t attr; pthread_t thread;
                int error=pthread_attr_init(&attr);
                if(!error) {
                    error=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
                    if(!error) error=pthread_attr_setstacksize(&attr,128*1024);
                    if(!error) error=pthread_create(&thread,&attr,start,t);
                    pthread_attr_destroy(&attr);
                }
                backend_fs_write(interrupted);
                if(error) {atomic_store(&t->tid,0);adopt_map_unbind(tls,t);result=-error;}
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
        backend_fs_write(self->resident_fs);
        g[REG_RDI]=(uintptr_t)self;
        g[REG_RSP]=self->resident_sp; g[REG_RIP]=(uintptr_t)adopt_exit_return;
        g[REG_RAX]=0; return;
    } else if(n==SYS_exit_group) R(exit_group,g[REG_RDI],0,0,0,0,0);
    else {
        /* The app may move its TLS base (glibc does this at startup). Rekey
         * the FS map before the syscall makes the new base live. */
        if(n==SYS_arch_prctl && g[REG_RDI]==0x1002) {
            uintptr_t previous=atomic_load(&self->app_fs);
            self->app_fs=g[REG_RSI];
            adopt_map_unbind(previous,self);
            adopt_map_bind(g[REG_RSI],self);
        }
        if(prior_sigsys.handler>1 && (prior_sigsys.flags&SA_SIGINFO)) {
            ((void (*)(int,siginfo_t *,void *))prior_sigsys.handler)(sig,info,opaque);
            return;
        }
        result=-ENOSYS;
    }
finish:
    g[REG_RAX]=result;
    return;
vfork_retry:
    /* The signal frame must be consumed before the kernel changes stacks.
     * No child-side C, FS switch, registry access or resident atfork operation. */
    atomic_fetch_add(&api.vforks,1);
    self->vfork_resume=g[REG_RIP];
    g[REG_RCX]=g[REG_RIP];
    g[REG_RIP]=(uintptr_t)adopt_vfork_entry;
    g[REG_RAX]=n;
}
void adopt_publish_exit(struct adopt_thread *self) {
    if(self->child_tid) {
        uintptr_t word=self->child_tid;
        atomic_store_explicit((_Atomic int *)word,0,memory_order_release);
        R(futex,word,FUTEX_WAKE,INT_MAX,0,0,0);
    }
}
static long identity(uintptr_t *p) {
    if(pthread_mutex_lock(&identity_mutex)) __builtin_trap();
    *p=(uintptr_t)pthread_self();
    long tid=gettid();
    if(pthread_mutex_unlock(&identity_mutex)) __builtin_trap();
    return tid;
}
_Noreturn void adopt_launch(uintptr_t entry,uintptr_t stack) {
    struct adopt_thread *t=adopt_current();
    t->regs[REG_RIP]=entry; t->regs[REG_RCX]=entry; t->regs[REG_RSP]=stack;
    t->regs[REG_EFL]=0x202;
    /* Initial linker entry does not inherit an application FP state. */
    __asm__ volatile("fxsave64 %0":"=m"(t->fp));
    t->xfeatures=0;
    app_brk=R(brk,0,0,0,0,0,0);
    backend_fs_write(0);
    adopt_run(t);
    signal_stack_release(t);
    adopt_map_unbind(t->resident_fs,t);
    adopt_map_unbind(atomic_load(&t->app_fs),t);
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
    adopt_map_configure(map_memory);
    fs_fast=(getauxval(AT_HWCAP2)&2)!=0;
    signal_stack_size=getauxval(AT_MINSIGSTKSZ);
    if(signal_stack_size>SIZE_MAX-65536) return -1;
    signal_stack_size+=65536;
    if(signal_stack_install(main)) return -1;
    main->resident_fs=backend_fs_read(); atomic_store(&main->tid,R(gettid,0,0,0,0,0,0));
    adopt_map_bind(main->resident_fs,main);
    if(pthread_atfork(resident_prepare,resident_parent,resident_child)) return -1;
    force_clone=getenv("MATON_ADOPT_FORCE_CLONE")!=NULL;
#ifndef MATON_GPU_BRIDGE
    char value[32]; snprintf(value,sizeof(value),"%lx",(unsigned long)&api);
    setenv("MATON_THREAD_ADOPT",value,1);
#endif
    /* The proxy owns seccomp routing; this handler adopts only Linux thread
     * ABI operations that require resident bookkeeping. */
    if(R(rt_sigaction,SIGSYS,0,&prior_sigsys,8,0,0)) return -1;
    struct {
        void (*handler)(int,siginfo_t *,void *);
        unsigned long flags;
        void (*restorer)(void);
        unsigned long mask;
    } action={trap,SA_SIGINFO|SA_ONSTACK|0x04000000UL,proxy_restore,0};
    if(R(rt_sigaction,SIGSYS,&action,0,8,0,0)) return -1;
    return 0;
}
