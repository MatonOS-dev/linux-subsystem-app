#ifndef MATON_VK_BACKEND_H
#define MATON_VK_BACKEND_H
#include <stddef.h>
#include <stdint.h>

/* Mirror the i386 provider's stable v2 prefix. Switch hooks are appended by
 * compat-proxy and own the reserved-GDT descriptor claim/release lifecycle. */
struct adopt_scope {
    void *thread;
    unsigned short previous_gs;
    unsigned active;
    unsigned already_android;
};
struct adopt_thread {
    int tid;
    uintptr_t android_tls;
    unsigned android_selector;
    unsigned scope_state;
    unsigned scope_depth;
};
struct adopt_api {
    unsigned version;
    struct adopt_thread *(*current)(void);
    struct adopt_thread *(*lookup)(uintptr_t);
    long (*identity)(uintptr_t *);
    unsigned clones, clone3s;
    size_t (*live)(void);
    unsigned fork_prepares, fork_parents, fork_children, vforks;
    int (*switch_in)(struct adopt_scope *);
    void (*switch_out)(struct adopt_scope *);
};
struct mv_scope { struct adopt_api *api; struct adopt_scope adopt; };

/* Linux i386 syscall ABI. Keep EBX/EBP intact for PIC code and place syscall
 * argument six in EBP from a stack-independent argument array. */
static inline long mv_sys(long n,long a,long b,long c,long d,long e,long f) {
    long args[6]={a,b,c,d,e,f}, result;
    __asm__ volatile("pushl %%ebx\n\tpushl %%ebp\n\t"
                     "pushl %1\n\tpushl %%eax\n\t"
                     "movl 4(%%esp), %%eax\n\t"
                     "movl 0(%%eax), %%ebx\n\t"
                     "movl 4(%%eax), %%ecx\n\t"
                     "movl 8(%%eax), %%edx\n\t"
                     "movl 12(%%eax), %%esi\n\t"
                     "movl 16(%%eax), %%edi\n\t"
                     "movl 20(%%eax), %%ebp\n\t"
                     "movl 0(%%esp), %%eax\n\t"
                     "int $0x80\n\t"
                     "addl $8, %%esp\n\tpopl %%ebp\n\tpopl %%ebx"
                     : "=a"(result) : "r"(args), "0"(n)
                     : "ecx","edx","esi","edi","memory","cc");
    return result;
}
static inline long mv_open(const char *p) { return mv_sys(5,(long)p,0,0,0,0,0); }
static inline long mv_open_write(const char *p,int mode) { return mv_sys(5,(long)p,1|64|512,mode,0,0,0); }
static inline long mv_write_fd(long fd,const void *p,size_t n) { return mv_sys(4,fd,(long)p,n,0,0,0); }
static inline long mv_read(long fd,void *p,size_t n) { return mv_sys(3,fd,(long)p,n,0,0,0); }
static inline void mv_close(long fd) { (void)mv_sys(6,fd,0,0,0,0,0); }
static inline int mv_socketpair(int pair[2]) {
    unsigned args[4]={1,5,0,(unsigned)(uintptr_t)pair};
    return (int)mv_sys(102,1,(long)args,0,0,0,0);
}
static inline void *mv_map(size_t n) {
    long p=mv_sys(192,0,n,3,0x22,(unsigned long)-1,0);
    return (unsigned long)p>=(unsigned long)-4095?0:(void *)(uintptr_t)p;
}
static inline void mv_unmap(void *p,size_t n) { (void)mv_sys(91,(long)p,n,0,0,0,0); }
static inline void mv_write(const char *s,size_t n) { (void)mv_sys(4,2,(long)s,n,0,0,0); }
static inline int mv_fast_tls(void) { return 0; }
static inline int switch_to_android(struct adopt_api *api,int fast,struct mv_scope *s) {
    (void)fast;
    s->api=api;
    return api && api->version==2 && api->switch_in && api->switch_out &&
           api->switch_in(&s->adopt)==0;
}
static inline void switch_back(struct mv_scope *s) {
    if (s->api && s->api->switch_out) s->api->switch_out(&s->adopt);
}
#endif
