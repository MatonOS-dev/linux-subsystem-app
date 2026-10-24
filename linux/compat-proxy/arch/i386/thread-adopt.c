#define _GNU_SOURCE
#include "adopt.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <unistd.h>
#ifdef MATON_GPU_BRIDGE
#include "../../proxy.h"
#include "../../vfs.h"
#include <ucontext.h>
#include <linux/futex.h>
#include <linux/sched.h>
#include <limits.h>
#endif

extern int proxy_tls_claim_reserved(const struct user_desc *, unsigned *);
extern int proxy_tls_release_reserved(void);

struct adopt_record {
    struct adopt_record *next;
    struct adopt_thread thread;
#ifdef MATON_GPU_BRIDGE
    uintptr_t resident_sp, child_tid, robust;
    unsigned long regs[19];
    struct user_desc app_tls;
    uint64_t mask;
    _Atomic int ready;
    _Alignas(64) unsigned char fp[16384];
    uint64_t xfeatures;
    uintptr_t app_base;
    long parent_tid;
#endif
};
static _Atomic(struct adopt_record *) records;
static struct adopt_api api;
#ifdef MATON_GPU_BRIDGE
static int resident_pid;
static struct adopt_record *record_alloc(void) {
    long p=RAW(PROXY_SC_mmap,0,sizeof(struct adopt_record),3,0x22,-1,0);
    return (unsigned long)p>=(unsigned long)-4095?NULL:(void *)p;
}
#else
static struct adopt_record *record_alloc(void) { return calloc(1,sizeof(struct adopt_record)); }
#endif

static int current_gs(void) {
    unsigned short value;
    __asm__ volatile("movw %%gs,%0" : "=rm"(value) :: "memory");
    return value;
}
static void load_gs(unsigned short value) {
    __asm__ volatile("movw %0,%%gs" :: "rm"(value) : "memory");
}
static int current_tid(void) {
#ifdef MATON_GPU_BRIDGE
    return (int)RAW(PROXY_SC_gettid,0,0,0,0,0,0);
#else
    return (int)syscall(SYS_gettid);
#endif
}

struct adopt_thread *adopt_current(void) {
    int tid = current_tid();
    for (struct adopt_record *r = atomic_load_explicit(&records,
             memory_order_acquire); r; r = r->next)
        if (atomic_load_explicit(&r->thread.tid, memory_order_acquire) == tid)
            return &r->thread;
#ifdef MATON_GPU_BRIDGE
    int pid=(int)RAW(PROXY_SC_getpid,0,0,0,0,0,0);
    if(resident_pid && pid!=resident_pid) {
        /* A libc fork retains its caller's TLS self pointer (GS:0 in both
         * bionic and glibc). Rebase the copied registry without a kernel query. */
        unsigned gs=(unsigned)current_gs();
        uintptr_t base=0;
        if(gs && !(gs&4)) {
            __asm__ volatile("movl %%gs:0,%0" : "=r"(base));
            struct adopt_record *self=NULL;
            for(struct adopt_record *r=atomic_load(&records);r;r=r->next)
                if(r->app_base==base || r->thread.android_tls==base)self=r;
            if(self) {
                for(struct adopt_record *r=atomic_load(&records);r;r=r->next)
                    atomic_store(&r->thread.tid,r==self?tid:0);
                resident_pid=pid;return &self->thread;
            }
        }
    }
#endif
    return NULL;
}

/* Called by the i386 clone/adoption path for each new resident thread. This
 * backend does not impose a thread-count cap: nodes remain published so a
 * signal-time reader can never observe freed storage. */
int adopt_register_current(uintptr_t android_tls) {
    if (!android_tls || android_tls > UINT32_MAX) return -EINVAL;
    int tid = current_tid();
    for (struct adopt_record *r = atomic_load_explicit(&records,
             memory_order_acquire); r; r = r->next) {
        if (atomic_load_explicit(&r->thread.tid, memory_order_acquire) == tid) {
            r->thread.android_tls = android_tls;
            return 0;
        }
    }
    struct adopt_record *fresh = record_alloc();
    if (!fresh) return -ENOMEM;
    fresh->thread.android_tls = android_tls;
#ifdef MATON_GPU_BRIDGE
    atomic_store_explicit(&fresh->thread.scope_state,2,memory_order_relaxed);
#endif
    atomic_store_explicit(&fresh->thread.tid, tid, memory_order_relaxed);
    struct adopt_record *head;
    do {
        head = atomic_load_explicit(&records, memory_order_acquire);
        fresh->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&records, &head, fresh,
                  memory_order_release, memory_order_acquire));
    return 0;
}

static long identity(uintptr_t *thread) {
    if (thread) *thread = (uintptr_t)pthread_self();
    return current_tid();
}
static size_t live(void) {
    size_t count = 0;
    for (struct adopt_record *r = atomic_load_explicit(&records,
             memory_order_acquire); r; r = r->next) {
        if (atomic_load_explicit(&r->thread.tid, memory_order_acquire) > 0) {
            if (count == SIZE_MAX) abort();
            count++;
        }
    }
    return count;
}
static struct adopt_api api = {
    .version = 2,
    .current = adopt_current,
    .lookup = NULL,
    .identity = identity,
    .live = live,
    .switch_in = adopt_switch_in,
    .switch_out = adopt_switch_out,
};
struct adopt_api *adopt_api_get(void) { return &api; }
uintptr_t adopt_api_address(void) { return (uintptr_t)&api; }

/* Bionic's x86 TLS ABI keeps the TLS block's own address in GS:0.
 * Android permits set_thread_area but not get_thread_area. In particular a
 * raw query from our gateway still traps in the inherited app filter. Read
 * the bionic ABI while GS is native; payload descriptors remain virtual. */
#ifdef MATON_GPU_BRIDGE
static uintptr_t native_tls(void) {
    uintptr_t base;
    __asm__ volatile("movl %%gs:0,%0" : "=r"(base));
    return base;
}
#endif
int adopt_install(void) {
    unsigned short gs = (unsigned short)current_gs();
    if (!gs || (gs & 4u)) return -ENOTSUP;
#ifdef MATON_GPU_BRIDGE
    int result=adopt_register_current(native_tls());
    extern int adopt_i386_enabled;
    adopt_i386_enabled=!result;
    resident_pid=(int)RAW(PROXY_SC_getpid,0,0,0,0,0,0);
#else
    struct user_desc descriptor = { .entry_number = gs >> 3 };
    if (syscall(244, &descriptor) < 0) return -errno;
    int result=adopt_register_current(descriptor.base_addr);
#endif
    return result;
}

int adopt_switch_in(struct adopt_scope *scope) {
    if (!scope) return -EINVAL;
    memset(scope, 0, sizeof(*scope));
    struct adopt_thread *thread = adopt_current();
    if (!thread) return -ENOENT;
    scope->thread = thread;
    scope->previous_gs = (unsigned short)current_gs();
    for (;;) {
        unsigned state = atomic_load_explicit(&thread->scope_state,
                                               memory_order_acquire);
        if (state == 2) {
            atomic_fetch_add_explicit(&thread->scope_depth, 1,
                                      memory_order_relaxed);
            scope->active = 1;
            scope->already_android = 1;
            return 0;
        }
        if (state == 1) return -EBUSY;
        unsigned expected = 0;
        if (atomic_compare_exchange_weak_explicit(&thread->scope_state,
                &expected, 1, memory_order_acq_rel, memory_order_acquire))
            break;
    }
    struct user_desc owner = {
        .entry_number = (unsigned)-1,
        .base_addr = (unsigned)thread->android_tls,
        .limit = 0xfffff,
        .seg_32bit = 1,
        .contents = 0,
        .read_exec_only = 0,
        .limit_in_pages = 1,
        .seg_not_present = 0,
        .useable = 1,
    };
    unsigned selector = 0;
    int result = proxy_tls_claim_reserved(&owner, &selector);
    if (result) {
        atomic_store_explicit(&thread->scope_state, 0, memory_order_release);
        return result < 0 ? result : -EIO;
    }
    if (selector > 0xffff || !(selector & 3u)) {
        (void)proxy_tls_release_reserved();
        atomic_store_explicit(&thread->scope_state, 0, memory_order_release);
        return -EINVAL;
    }
    thread->android_selector = selector;
    /* The reserved selector may equal the payload's virtual third slot.
     * Clear the segment register first so the processor reloads its descriptor
     * even when the selector value is unchanged. */
    load_gs(0);
    load_gs((unsigned short)selector);
    atomic_store_explicit(&thread->scope_depth, 1, memory_order_relaxed);
    atomic_store_explicit(&thread->scope_state, 2, memory_order_release);
    scope->active = 1;
    return 0;
}

void adopt_switch_out(struct adopt_scope *scope) {
    if (!scope || !scope->active) return;
    if (scope->already_android) {
        atomic_fetch_sub_explicit(&scope->thread->scope_depth, 1,
                                  memory_order_relaxed);
    } else {
        atomic_store_explicit(&scope->thread->scope_state, 1,
                              memory_order_release);
        atomic_store_explicit(&scope->thread->scope_depth, 0,
                              memory_order_relaxed);
        if (proxy_tls_release_reserved()) abort();
        /* set_thread_area updates the descriptor table, not the CPU's hidden
         * GS cache. Reload the saved selector after release to make the
         * restored application descriptor active in that cache. */
        load_gs(0);
        load_gs(scope->previous_gs);
        atomic_store_explicit(&scope->thread->scope_state, 0,
                              memory_order_release);
    }
    scope->active = 0;
}

#ifdef MATON_GPU_BRIDGE
int adopt_i386_enabled;
extern void adopt_i386_run(unsigned long *,uintptr_t *,void *,uint64_t);
extern void adopt_i386_return(void);
extern int proxy_i386_tls_start(const struct user_desc *,unsigned,long);
extern void proxy_i386_thread_exit(void);
extern int proxy_i386_tls_enter(unsigned);
int adopt_i386_native(void) {
    struct adopt_thread *t=adopt_current();
    return !t || atomic_load_explicit(&t->scope_state,memory_order_acquire)==2;
}
static struct adopt_record *record_current(void) {
    struct adopt_thread *t=adopt_current();
    return t?(struct adopt_record *)((char *)t-
                offsetof(struct adopt_record,thread)):NULL;
}
static void owner_died(uintptr_t entry,long offset,int tid) {
    if(!entry || (entry&1))return; /* PI robust mutexes need kernel support. */
    _Atomic unsigned *word=(_Atomic unsigned *)(entry+offset);
    unsigned old;
    if(proxy_copy(&old,word,sizeof(old)))return;
    while((old&FUTEX_TID_MASK)==(unsigned)tid) {
        unsigned next=(old&FUTEX_WAITERS)|FUTEX_OWNER_DIED;
        if(atomic_compare_exchange_weak(word,&old,next)) {
            if(old&FUTEX_WAITERS)RAW(PROXY_SC_futex,word,FUTEX_WAKE,INT_MAX,0,0,0);
            break;
        }
    }
}
static void robust_exit(struct adopt_record *r) {
    struct robust_list_head *head=(void *)r->robust,copy;
    if(!head || proxy_copy(&copy,head,sizeof(copy)))return;
    uintptr_t pending=(uintptr_t)copy.list_op_pending;
    struct robust_list *p=copy.list.next;
    for(unsigned count=0;p && p!=&head->list && count<2048;count++) {
        uintptr_t entry=(uintptr_t)p;
        if(proxy_copy(&p,(void *)(entry&~1u),sizeof(p)))return;
        if(entry!=pending)owner_died(entry,copy.futex_offset,current_tid());
    }
    owner_died(pending,copy.futex_offset,current_tid());
}
static void record_exit(struct adopt_record *r) {
    proxy_i386_thread_exit();
    if(r->child_tid) {
        atomic_store_explicit((_Atomic int *)r->child_tid,0,memory_order_release);
        RAW(PROXY_SC_futex,r->child_tid,FUTEX_WAKE,INT_MAX,0,0,0);
    }
    atomic_store_explicit(&r->thread.tid,0,memory_order_release);
}
static void *thread_start(void *arg) {
    struct adopt_record *r=arg;
    unsigned gs=(unsigned)current_gs();
    if(adopt_register_current(native_tls()))
        RAW(PROXY_SC_exit_group,193,0,0,0,0,0);
    /* Register uses its own published node. Copy the kernel-ABI launch data
     * into that node while all allocation and pthread calls still use bionic. */
    struct adopt_record *self=record_current();
    self->app_tls=r->app_tls;self->mask=r->mask;self->child_tid=r->child_tid;
    p_copy(self->regs,r->regs,sizeof(self->regs));
    p_copy(self->fp,r->fp,sizeof(self->fp));self->xfeatures=r->xfeatures;
    self->app_base=self->app_tls.base_addr;self->parent_tid=r->parent_tid;
    atomic_store_explicit(&r->thread.tid,current_tid(),memory_order_relaxed);
    atomic_store_explicit(&r->ready,1,memory_order_release);
    RAW(PROXY_SC_futex,&r->ready,FUTEX_WAKE,INT_MAX,0,0,0);
    while(atomic_load_explicit(&r->ready,memory_order_acquire)!=2)
        RAW(PROXY_SC_futex,&r->ready,FUTEX_WAIT,1,0,0,0);
    /* From this point no libc runs until the exit gate restores bionic GS. */
    if(proxy_i386_tls_start(&self->app_tls,gs,self->parent_tid))
        RAW(PROXY_SC_exit_group,194,0,0,0,0,0);
    atomic_store_explicit(&r->ready,3,memory_order_release);
    RAW(PROXY_SC_futex,&r->ready,FUTEX_WAKE,INT_MAX,0,0,0);
    RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&self->mask,0,8,0,0);
    atomic_store_explicit(&self->thread.scope_state,0,memory_order_release);
    load_gs((unsigned short)((self->app_tls.entry_number<<3)|3));
    adopt_i386_run(self->regs,&self->resident_sp,self->fp,self->xfeatures);
    record_exit(self);
    return NULL;
}
static long thread_clone(ucontext_t *uc,proxy_arg_t *a) {
    unsigned flags=(unsigned)a[0];
    unsigned required=CLONE_VM|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS|
                      CLONE_FS|CLONE_FILES|CLONE_SYSVSEM;
    unsigned allowed=required|CLONE_DETACHED|CLONE_PARENT_SETTID|
                     CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID;
    if((flags&required)!=required || (flags&~allowed) || !a[1])return -ENOSYS;
    struct user_desc tls;
    if(proxy_copy(&tls,(void *)(uintptr_t)a[3],sizeof(tls)))return -EFAULT;
    if(tls.entry_number<3 || tls.entry_number>INT32_MAX || !tls.seg_32bit ||
       tls.contents || tls.seg_not_present)return -EINVAL;
    const unsigned char *fp=(void *)uc->uc_mcontext.fpregs;
    if(!fp || *(const unsigned short *)(fp+110)!=0)return -EOPNOTSUPP;
    size_t fp_size=512;uint64_t features=0;
    /* IA32 signal frames prepend the 112-byte FSAVE header to the
     * FXSAVE/XSAVE area. The software trailer describes extended state. */
    if(*(const unsigned *)(fp+112+464)==0x46505853) {
        fp_size=*(const unsigned *)(fp+112+480);
        features=*(const uint64_t *)(fp+112+472);
    }
    if(fp_size<512 || fp_size>sizeof(((struct adopt_record *)0)->fp))return -EOPNOTSUPP;
    struct adopt_scope scope;
    if(adopt_switch_in(&scope))return -ENOENT;
    struct adopt_record *r=record_alloc();
    int error=r?0:ENOMEM;
    if(!error) {
        r->parent_tid=current_tid();
        r->app_tls=tls;r->child_tid=(flags&CLONE_CHILD_CLEARTID)?a[4]:0;
        p_copy(r->regs,uc->uc_mcontext.gregs,sizeof(r->regs));
        r->regs[REG_ESP]=a[1];r->regs[REG_UESP]=a[1];r->regs[REG_EAX]=0;
        p_copy(&r->mask,&uc->uc_sigmask,8);
        p_copy(r->fp,fp+112,fp_size);r->xfeatures=features;
        pthread_attr_t attr;pthread_t thread;
        error=pthread_attr_init(&attr);
        if(!error) {
            error=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
            if(!error)error=pthread_attr_setstacksize(&attr,128*1024);
            if(!error)error=pthread_create(&thread,&attr,thread_start,r);
            pthread_attr_destroy(&attr);
        }
    }
    adopt_switch_out(&scope);
    if(error)return -error;
    while(!atomic_load_explicit(&r->ready,memory_order_acquire))
        RAW(PROXY_SC_futex,&r->ready,FUTEX_WAIT,0,0,0,0);
    int tid=atomic_load_explicit(&r->thread.tid,memory_order_relaxed);
    if(flags&CLONE_PARENT_SETTID)*(int *)(uintptr_t)a[2]=tid;
    if(flags&CLONE_CHILD_SETTID)*(int *)(uintptr_t)a[4]=tid;
    atomic_store_explicit(&r->ready,2,memory_order_release);
    RAW(PROXY_SC_futex,&r->ready,FUTEX_WAKE,INT_MAX,0,0,0);
    atomic_fetch_add_explicit(&api.clones,1,memory_order_relaxed);
    while(atomic_load_explicit(&r->ready,memory_order_acquire)!=3)
        RAW(PROXY_SC_futex,&r->ready,FUTEX_WAIT,2,0,0,0);
    RAW(PROXY_SC_munmap,r,sizeof(*r),0,0,0,0);
    return tid;
}
/* Called only for payload sites by the proxy's common SIGSYS dispatcher.
 * Android library syscalls have already taken their native route. */
int adopt_i386_syscall(long nr,proxy_arg_t *a,void *opaque,long *result) {
    if(!adopt_i386_enabled)return 0;
    ucontext_t *uc=opaque;
    struct adopt_record *r=record_current();
    if(!r)return 0;
    struct user_desc current={.entry_number=(unsigned)current_gs()>>3};
    extern int proxy_i386_tls_current(struct user_desc *);
    if(!proxy_i386_tls_current(&current))r->app_base=current.base_addr;
    if(nr==PROXY_SC_clone && (a[0]&CLONE_THREAD)) {
        *result=thread_clone(uc,a);return 1;
    }
    if(nr==PROXY_SC_set_robust_list) {
        if(a[1]!=sizeof(struct robust_list_head))*result=-EINVAL;
        else {r->robust=a[0];*result=0;}
        return 1;
    }
    if(nr==PROXY_SC_get_robust_list) {
        if(a[0] && a[0]!=current_tid())*result=-EOPNOTSUPP;
        else {
            uintptr_t size=sizeof(struct robust_list_head);
            *result=proxy_copy((void *)(uintptr_t)a[1],&r->robust,sizeof(r->robust)) ||
                    proxy_copy((void *)(uintptr_t)a[2],&size,sizeof(size))?-EFAULT:0;
        }
        return 1;
    }
    if(nr==PROXY_SC_set_tid_address) {
        r->child_tid=a[0];*result=current_tid();return 1;
    }
    if(nr==PROXY_SC_exit && r->resident_sp) {
        robust_exit(r);
        struct adopt_scope scope;
        if(adopt_switch_in(&scope))RAW(PROXY_SC_exit_group,195,0,0,0,0,0);
        uc->uc_mcontext.gregs[REG_GS]=r->thread.android_selector;
        uc->uc_mcontext.gregs[REG_ESP]=r->resident_sp;
        uc->uc_mcontext.gregs[REG_UESP]=r->resident_sp;
        uc->uc_mcontext.gregs[REG_EIP]=(uintptr_t)adopt_i386_return;
        *result=0;return 1;
    }
    return 0;
}
_Noreturn void adopt_launch(uintptr_t entry, uintptr_t stack) {
    struct adopt_record *r=record_current();
    if(!r)RAW(PROXY_SC_exit_group,196,0,0,0,0,0);
    r->regs[REG_EIP]=entry;r->regs[REG_ESP]=stack;
    r->regs[REG_EFL]=0x202;
    __asm__ volatile("fxsave %0":"=m"(r->fp));
    r->xfeatures=0;
    unsigned native_gs=(unsigned)current_gs();
    atomic_store_explicit(&r->thread.scope_state,0,memory_order_release);
    if(proxy_i386_tls_enter(native_gs))
        RAW(PROXY_SC_exit_group,197,0,0,0,0,0);
    adopt_i386_run(r->regs,&r->resident_sp,r->fp,r->xfeatures);
    record_exit(r);
    pthread_exit(NULL);
}
#endif
