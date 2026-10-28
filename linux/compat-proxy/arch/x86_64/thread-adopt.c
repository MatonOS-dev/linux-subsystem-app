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
#include "../../vfs.h"
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
#include "../../resident-pthread.h"
extern long proxy_raw(long,long,long,long,long,long,long);
extern long proxy_sigaltstack_on_stack(stack_t *,void *);
extern void proxy_log(const char *,long,long);
extern int proxy_trace;
extern char proxy_clone_entry[];
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
static _Atomic int exec_owner;
/* Two independent glibc allocators cannot share sbrk: one can shrink the
 * other's live heap. Freeze only the app's program break; its malloc uses
 * mmap fallback without an imposed memory cap. Resident brk remains native. */
static uintptr_t app_brk;
static int force_clone;
static size_t live(void);
static size_t signal_stack_size;
static long identity(uintptr_t *);
static void fork_rebase(struct adopt_thread *);
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
 * the launcher/ICD publish the same bit. rdfsbase/wrfsbase are then register
 * accesses with no syscall; without it we fall back to arch_prctl through the
 * always-allowed raw gateway. */
static uintptr_t backend_fs_read(void) {
    uintptr_t fs=0;
    if(fs_fast) { __asm__ volatile("rdfsbase %0":"=r"(fs)::"memory"); return fs; }
    if(R(arch_prctl,0x1003,&fs,0,0,0,0)) __builtin_trap();
    return fs;
}
static void backend_fs_write(uintptr_t fs) {
    if(fs_fast) { __asm__ volatile("wrfsbase %0"::"r"(fs):"memory"); return; }
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
    /* A proxy child can reach a fresh ELF's first constructor before its
     * clone continuation is rebased. If it inherited exactly one live record,
     * that record is unambiguously the calling thread; refresh its identity
     * now and repair the FS map for the post-exec TLS base. */
    struct adopt_thread *only=NULL;
    for(struct thread_chunk *c=atomic_load_explicit(&chunks,memory_order_acquire);c;
        c=atomic_load_explicit(&c->next,memory_order_acquire))
        for(unsigned i=0;i<CHUNK_THREADS;i++) {
            struct adopt_thread *candidate=&c->entries[i];
            int candidate_tid=atomic_load_explicit(&candidate->tid,memory_order_acquire);
            if(candidate_tid>0) {
                if(only) return NULL;
                only=candidate;
            }
        }
    if(only) {
        uintptr_t old_app_fs=atomic_load_explicit(&only->app_fs,memory_order_acquire);
        backend_fs_write(only->resident_fs);
        fork_rebase(only);
        identity_mutex=(pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
        adopt_map_unbind(old_app_fs,only);
        atomic_store_explicit(&only->app_fs,fs,memory_order_release);
        adopt_map_bind(fs,only);
        backend_fs_write(fs);
        proxy_log("adopt-recovered-fork-child",tid,(long)fs);
        return only;
    }
    return NULL;
}
/* The syscall proxy permits Android graphics/HAL passthrough only while this
 * Linux thread is actually executing with its resident Android TLS. */
int adopt_resident_tls_active(void) {
    uintptr_t fs=backend_fs_read();
    /* This query runs in the syscall signal handler. A miss must not enter
     * adopt_current's fork-child recovery path or mutate the thread registry. */
    struct adopt_thread *thread=adopt_map_lookup(fs);
    if (!thread) return 0;
    if (thread->resident_fs==fs) return 1;
    if (atomic_load_explicit(&thread->app_fs,memory_order_acquire)==fs) return 2;
    return 3;
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
                atomic_store(&c->entries[i].payload,0);
                atomic_store(&c->entries[i].exec_kicked,0);
                return &c->entries[i];
            }
        }
        link=&c->next;
    }
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
    atomic_store(&exec_owner,0);
    atomic_store(&self->exec_kicked,0);
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
    /* Linux clears the calling thread's alternate signal stack in a
     * non-CLONE_VM process child. The proxy's SIGSYS action uses SA_ONSTACK,
     * so register a valid stack again before returning to payload code.
     * Preserve the kernel's post-fork prior state for normal teardown. */
    stack_t current;
    long query_result=R(sigaltstack,0,&current,0,0,0,0);
    if(query_result) {
        proxy_log("adopt-fork-altstack-query",R(gettid,0,0,0,0,0,0),-query_result);
        self->fork_context=NULL;
        return;
    }
    current.ss_flags&=~SS_ONSTACK;
    self->prior_signal_stack=current;
    uintptr_t stack_pointer;
    __asm__ volatile("mov %%rsp,%0":"=r"(stack_pointer));
    void *signal_stack=self->signal_stack;
    uintptr_t inherited=(uintptr_t)current.ss_sp;
    uintptr_t saved=(uintptr_t)signal_stack;
    if((!(current.ss_flags&SS_DISABLE) && stack_pointer>=inherited &&
        stack_pointer-inherited<current.ss_size) ||
       (stack_pointer>=saved && stack_pointer-saved<signal_stack_size)) {
        /* A stackless fork can be adopted while its SIGSYS frame is still on
         * the inherited stack. Register a fresh stack so that this live
         * frame remains untouched until rt_sigreturn consumes it. */
        long mapping=R(mmap,0,signal_stack_size,PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if((unsigned long)mapping>=(unsigned long)-4095) {
            proxy_log("adopt-fork-altstack-map",R(gettid,0,0,0,0,0,0),mapping);
            self->fork_context=NULL;
            return;
        }
        signal_stack=(void *)mapping;
    }
    stack_t resident={.ss_sp=signal_stack,.ss_size=signal_stack_size};
    /* Fork adoption runs inside the proxy's SIGSYS handler. The kernel
     * rejects sigaltstack changes while RSP is on the currently registered
     * stack (EPERM), even when the child is installing a fresh mapping.
     * Execute the raw syscall on the new mapping so the current RSP is outside
     * the old stack; the live signal frame remains on the inherited mapping
     * until sigreturn consumes it. */
    long stack_result=R(sigaltstack,&resident,0,0,0,0,0);
    /* Retry continuations are already outside the inherited stack. Moving
     * those onto that same registered stack would itself cause EPERM.
     * Only an active-frame adoption needs the fresh-stack trampoline. */
    if(stack_result==-EPERM && signal_stack!=self->signal_stack)
        stack_result=proxy_sigaltstack_on_stack(&resident,
                        (char *)signal_stack+signal_stack_size-128);
    if(stack_result) {
        proxy_log("adopt-fork-altstack-install",R(gettid,0,0,0,0,0,0),
                  -stack_result);
        if(signal_stack!=self->signal_stack) R(munmap,signal_stack,signal_stack_size,0,0,0,0);
    } else {
        self->signal_stack=signal_stack;
        /* rt_sigreturn restores uc_stack from the live frame. Keep it in
         * sync with the kernel registration or it will undo the install. */
        ucontext_t *context=self->fork_context;
        if(context) {
            context->uc_stack.ss_sp=signal_stack;
            context->uc_stack.ss_size=signal_stack_size;
            context->uc_stack.ss_flags=0;
        }
    }
    self->fork_context=NULL;
}
/* Called by the proxy's successful process-clone continuation, while the
 * child still has the application's FS base. Keep this hook x86_64-local:
 * the freestanding proxy links a weak no-op when adoption is absent. */
__attribute__((target("general-regs-only")))
void adopt_fork_child(void) {
    uintptr_t app_fs=backend_fs_read();
    /* After a process clone, the kernel has changed gettid() but the copied
     * registry still contains the parent's TID. The child inherits the
     * calling thread's FS base, so use that stable identity to find its
     * copied record before fork_rebase() publishes the child's new TID. */
    struct adopt_thread *self=adopt_map_lookup(app_fs);
    if(!self) {
        for(struct thread_chunk *c=atomic_load_explicit(&chunks,memory_order_acquire);c && !self;
            c=atomic_load_explicit(&c->next,memory_order_acquire))
            for(unsigned i=0;i<CHUNK_THREADS;i++) {
                struct adopt_thread *candidate=&c->entries[i];
                if(atomic_load_explicit(&candidate->tid,memory_order_acquire)>0 &&
                   atomic_load_explicit(&candidate->app_fs,memory_order_acquire)==app_fs) {
                    self=candidate;
                    break;
                }
            }
    }
    if(!self) {
        proxy_log("adopt-fork-miss",R(gettid,0,0,0,0,0,0),(long)app_fs);
        __builtin_trap();
    }
    backend_fs_write(self->resident_fs);
    fork_rebase(self);
    proxy_log("adopt-fork-child",atomic_load_explicit(&self->tid,memory_order_acquire),
              (long)atomic_load_explicit(&self->app_fs,memory_order_acquire));
    /* Raw proxy clones bypass libc's pthread_atfork handlers. Discard a mutex
     * possibly owned by a vanished thread before identity() can use it. */
    identity_mutex=(pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    backend_fs_write(app_fs);
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
    vfs_trap_thread_exit(t->signal_stack,signal_stack_size);
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
/* Exec retirement uses our existing thread-exit gate, never pthread_cancel
 * or raw exit with a live resident pthread record. SIGSYS is reserved and
 * cannot be blocked by the payload. A thread interrupted inside resident
 * code finishes that call and retires at its next payload trap. */
int adopt_exec_active(void){return atomic_load(&chunks)!=NULL;}
int adopt_exec_retire(void) {
    struct adopt_thread *caller=adopt_current(); /* Rebase after fork. */
    /* proxy_load also stages the initial image, before any payload exists. */
    if(!caller || !atomic_load(&caller->payload))return 0;
    int self=R(gettid,0,0,0,0,0,0),zero=0;
    if(!atomic_compare_exchange_strong(&exec_owner,&zero,self) && zero!=self) {
        /* The competing exec's kick will take this caller through its exit
         * gate after the current signal frame returns. */
        return -EINTR;
    }
    int pending;
    do {
        pending=0;
        for(struct thread_chunk *c=atomic_load(&chunks);c;c=atomic_load(&c->next))
            for(unsigned i=0;i<CHUNK_THREADS;i++) {
                struct adopt_thread *t=&c->entries[i];int tid=atomic_load(&t->tid);
                if(!tid || tid==self || !atomic_load(&t->payload))continue;
                pending=1;
                if(tid>0 && !atomic_exchange(&t->exec_kicked,1))
                    R(tgkill,R(getpid,0,0,0,0,0,0),tid,SIGSYS,0,0,0);
            }
        if(pending)R(sched_yield,0,0,0,0,0,0);
    } while(pending);
    atomic_store(&exec_owner,0);
    return 0;
}
static int exec_retire_signal(void *opaque) {
    int owner=atomic_load(&exec_owner);
    if(!owner || owner==R(gettid,0,0,0,0,0,0))return 0;
    /* Signal-time lookup must not run fork recovery on resident workers. */
    int tid=R(gettid,0,0,0,0,0,0);
    struct adopt_thread *self=NULL;
    for(struct thread_chunk *c=atomic_load(&chunks);c;c=atomic_load(&c->next))
        for(unsigned i=0;i<CHUNK_THREADS;i++)
            if(atomic_load(&c->entries[i].tid)==tid)self=&c->entries[i];
    if(!self || !atomic_load(&self->payload) || !atomic_load(&exec_owner))return 0;
    /* Do not abandon a resident library with its locks held. */
    if(backend_fs_read()==self->resident_fs || (((ucontext_t *)opaque)->uc_stack.ss_flags&SS_ONSTACK)){
        /* Acknowledge this wakeup. Keep at most one kick outstanding, and
         * retry after the resident call/clone startup has made progress. */
        atomic_store(&self->exec_kicked,0);return 0;
    }
    robust_exit(self);
    uint64_t block=UINT64_C(1)<<(SIGCHLD-1);
    R(rt_sigprocmask,SIG_BLOCK,&block,0,8,0,0);
    ucontext_t *uc=opaque;greg_t *g=uc->uc_mcontext.gregs;
    *(uint64_t *)&uc->uc_sigmask|=block;
    backend_fs_write(self->resident_fs);
    g[REG_RDI]=(uintptr_t)self;g[REG_RSP]=self->resident_sp;
    g[REG_RIP]=(uintptr_t)adopt_exit_return;g[REG_RAX]=0;
    return 1;
}
static void trap(int sig,siginfo_t *info,void *opaque) {
    if(info->si_code!=SYS_SECCOMP){uint64_t block=UINT64_C(1)<<(SIGSYS-1);R(rt_sigprocmask,SIG_BLOCK,&block,0,8,0,0);}
    if(exec_retire_signal(opaque))return;
    if(info->si_code==SI_TKILL && info->si_pid==R(getpid,0,0,0,0,0,0) && atomic_load(&exec_owner))return;
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
       n!=SYS_clone && n!=SYS_clone3 && n!=SYS_vfork &&
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
    if(!self) {
        uintptr_t fs=backend_fs_read();
        long tid=R(gettid,0,0,0,0,0,0);
        proxy_log("adopt-miss-tid",tid,(long)fs);
        proxy_log("adopt-miss-map",(long)adopt_map_lookup(fs),(long)live());
        for(struct thread_chunk *c=atomic_load_explicit(&chunks,memory_order_acquire);c;
            c=atomic_load_explicit(&c->next,memory_order_acquire))
            for(unsigned i=0;i<CHUNK_THREADS;i++) {
                struct adopt_thread *candidate=&c->entries[i];
                int candidate_tid=atomic_load_explicit(&candidate->tid,memory_order_acquire);
                if(candidate_tid) {
                    proxy_log("adopt-miss-record",candidate_tid,
                              (long)atomic_load_explicit(&candidate->app_fs,memory_order_acquire));
                    goto miss_record_logged;
                }
            }
miss_record_logged:
        if(prior_sigsys.handler>1 && (prior_sigsys.flags&SA_SIGINFO)) {
            ((void (*)(int,siginfo_t *,void *))prior_sigsys.handler)(sig,info,opaque);
            return;
        }
        R(exit_group,190,0,0,0,0,0);
        __builtin_unreachable();
    }
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
        if(g[REG_RDI]==SIGSYS) {
            /* The proxy owns the kernel SIGSYS action and emulates payload
             * registrations by chaining them. Let its dispatcher update the
             * chained action; rejecting this here makes apps that install a
             * SIGSYS handler after fork/exec fail their signal setup. */
            if(prior_sigsys.handler>1 && (prior_sigsys.flags&SA_SIGINFO)) {
                ((void (*)(int,siginfo_t *,void *))prior_sigsys.handler)(sig,info,opaque);
                return;
            }
            result=-EINVAL;
        } else {
            result=R(rt_sigaction,g[REG_RDI],g[REG_RSI],g[REG_RDX],g[REG_R10],0,0);
            if(proxy_trace && g[REG_RDI]==SIGCHLD){
                struct {uintptr_t handler,flags,restorer;uint64_t mask;} current={0};
                R(rt_sigaction,SIGCHLD,0,&current,8,0,0);
                proxy_log("adopt-chld-action",current.handler,current.flags);
            }
        }
    } else if(n==SYS_rt_sigprocmask) {
        /* The dispatcher owns the payload's virtual SIGSYS mask as well as
         * its disposition. Never expose the resident handler's kernel mask. */
        if(prior_sigsys.handler>1 && (prior_sigsys.flags&SA_SIGINFO)) {
            ((void (*)(int,siginfo_t *,void *))prior_sigsys.handler)(sig,info,opaque);
            return;
        }
        result=-EINVAL;
    }
    else if(n==SYS_clone3 || n==SYS_clone) {
        unsigned long flags,stack,tls,ptid,ctid;
        unsigned long exit_signal;
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
        if(!(flags&CLONE_THREAD)) {
            if(prior_sigsys.handler>1 && (prior_sigsys.flags&SA_SIGINFO)) {
                self->fork_context=uc;
                ((void (*)(int,siginfo_t *,void *))prior_sigsys.handler)(sig,info,opaque);
                /* In-handler fork paths return through this signal frame.
                 * Retry clones hand off to proxy_clone_entry and rebase in
                 * assembly, with the encoded syscall number in RAX. */
                if(g[REG_RAX]==0) adopt_fork_child();
                else self->fork_context=NULL;
                return;
            }
            result=-ENOSYS;goto finish;
        }
        if(exit_signal) {result=REJECT("unsupported process clone/signal",EINVAL);goto finish;}
        unsigned long required=CLONE_VM|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS|CLONE_FS|CLONE_FILES|CLONE_SYSVSEM;
        unsigned long allowed=required|CLONE_DETACHED|CLONE_FS|CLONE_FILES|CLONE_SYSVSEM|CLONE_PARENT_SETTID|CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID;
        if((flags&required)!=required || (flags&~allowed)) result=REJECT("thread flags",ENOSYS);
        else {
            struct adopt_thread *t=reserve();
            if(!t) result=-EAGAIN;
            else {
                atomic_store(&t->payload,1);
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
    } else if(n==SYS_vfork) {
        if(prior_sigsys.handler>1 && (prior_sigsys.flags&SA_SIGINFO)) {
            self->fork_context=uc;
            ((void (*)(int,siginfo_t *,void *))prior_sigsys.handler)(sig,info,opaque);
            if(g[REG_RAX]==0) adopt_fork_child();
            else self->fork_context=NULL;
            return;
        }
        result=-ENOSYS;
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
        *(uint64_t *)&uc->uc_sigmask|=UINT64_C(1)<<(SIGCHLD-1);
        robust_exit(self);
        /* Consume the signal frame before publishing exit: join may unmap
         * the entire application stack as soon as the tid becomes zero. */
        backend_fs_write(self->resident_fs);
        g[REG_RDI]=(uintptr_t)self;
        g[REG_RSP]=self->resident_sp; g[REG_RIP]=(uintptr_t)adopt_exit_return;
        g[REG_RAX]=0; return;
    } else if(n==SYS_exit_group) {
        if(proxy_trace)proxy_log("adopt-exit-group",R(getpid,0,0,0,0,0,0),g[REG_RDI]);
        R(exit_group,g[REG_RDI],0,0,0,0,0);
    }
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
    atomic_store(&t->payload,1);
    t->regs[REG_RIP]=entry; t->regs[REG_RCX]=entry; t->regs[REG_RSP]=stack;
    t->regs[REG_EFL]=0x202;
    /* Initial linker entry does not inherit an application FP state. */
    __asm__ volatile("fxsave64 %0":"=m"(t->fp));
    t->xfeatures=0;
    app_brk=R(brk,0,0,0,0,0,0);
    backend_fs_write(0);
    adopt_run(t);
    vfs_trap_thread_exit(t->signal_stack,signal_stack_size);
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
    } action={trap,SA_SIGINFO|SA_ONSTACK|SA_NODEFER|0x04000000UL,proxy_restore,0};
    if(R(rt_sigaction,SIGSYS,&action,0,8,0,0)) return -1;
    return 0;
}
