#ifndef MATON_VK_BACKEND_H
#define MATON_VK_BACKEND_H
#include <stdint.h>
/* v1 adoption ABI prefix, mirrored without any libc headers. A host test
 * checks offsets against the provider's actual definition. Never inspect TCBs. */
struct adopt_thread {
    int tid; uintptr_t resident_fs, app_fs, resident_sp, child_tid;
    unsigned long robust; int ready; long regs[23]; unsigned long mask;
    _Alignas(64) unsigned char fp[16384]; uint64_t xfeatures;
    unsigned char selector;
};
struct adopt_api { unsigned version; struct adopt_thread *(*current)(void);
                  struct adopt_thread *(*lookup)(uintptr_t fs); };
static inline long mv_sys(long n,long a,long b,long c,long d,long e,long f) {
    register long r10 __asm__("r10")=d;
    register long r8 __asm__("r8")=e;
    register long r9 __asm__("r9")=f;
    long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");
    return r;
}
static inline long mv_open(const char *p) {return mv_sys(257,-100,(long)p,0,0,0,0);}
static inline long mv_read(long fd,void *p,size_t n) {return mv_sys(0,fd,(long)p,n,0,0,0);}
static inline void mv_close(long fd) {mv_sys(3,fd,0,0,0,0,0);}
static inline void *mv_map(size_t n) {
    long p=mv_sys(9,0,n,3,0x22,-1,0);
    return (unsigned long)p>=(unsigned long)-4095?0:(void *)p;
}
static inline void mv_unmap(void *p,size_t n) { mv_sys(11,(long)p,n,0,0,0,0); }
static inline void mv_write(const char *s,size_t n) { mv_sys(1,2,(long)s,n,0,0,0); }
static inline int mv_fast_tls(void) {
    long fd=mv_sys(257,-100,(long)"/proc/self/auxv",0,0,0,0);
    if(fd<0) return 0;
    uintptr_t pair[2]; int fast=0;
    while(mv_sys(0,fd,(long)pair,sizeof(pair),0,0,0)==sizeof(pair))
        if(pair[0]==26) {fast=!!(pair[1]&2);break;}
    mv_sys(3,fd,0,0,0,0,0); return fast;
}
static inline uintptr_t mv_tp_read(int fast) {
    uintptr_t p;
    if(fast) __asm__ volatile("rdfsbase %0":"=r"(p)::"memory");
    else if(mv_sys(158,0x1003,(long)&p,0,0,0,0)) __builtin_trap();
    return p;
}
static inline void mv_tp_write(uintptr_t p,int fast) {
    if(fast) __asm__ volatile("wrfsbase %0"::"r"(p):"memory");
    else if(mv_sys(158,0x1002,p,0,0,0,0)) __builtin_trap();
}
struct mv_scope { uintptr_t previous; struct adopt_thread *thread; unsigned char selector; int fast; };
static inline int switch_to_android(struct adopt_api *api,int fast,struct mv_scope *s) {
    if(!api) return 0;
    /* v2: O(1) syscall-free lookup keyed on the current FS base. FS is only
     * read with rdfsbase (`fast`); the arch_prctl read is itself a syscall and
     * would trap here, because user dispatch is still filtering at this point.
     * A miss falls back to the tid scan so a provider that has not bound this
     * thread yet (or an FS base of 0 during startup) still works. */
    s->fast=fast;
    int have_previous=0;
    if(api->version==2 && api->lookup && fast) {
        uintptr_t fs=mv_tp_read(1);
        s->thread=api->lookup(fs);
        if(!s->thread) s->thread=api->current();
        s->previous=fs; have_previous=1;
    } else if((api->version==1 || api->version==2) && api->current) {
        s->thread=api->current();
        if(!s->thread) return 0;
    } else return 0;
    if(!s->thread) return 0;
    /* The user-dispatch selector is read only on this thread: by the kernel at
     * syscall entry and by our SIGSYS handler. Program order suffices, so use
     * plain stores with a compiler barrier, not a locked/fenced read-modify-
     * write. See Documentation/admin-guide/syscall-user-dispatch.rst. */
    s->selector=__atomic_load_n(&s->thread->selector,__ATOMIC_RELAXED);
    __atomic_store_n(&s->thread->selector,0,__ATOMIC_RELAXED);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    /* Only now may the slow backend read FS without trapping. */
    if(!have_previous) s->previous=mv_tp_read(fast);
    mv_tp_write(s->thread->resident_fs,fast); return 1;
}
static inline void switch_back(struct mv_scope *s) {
    mv_tp_write(s->previous,s->fast);
    __atomic_store_n(&s->thread->selector,s->selector,__ATOMIC_RELAXED);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
}
#endif
