/* Freestanding application: no libc, pthread code, allocation helpers or TLS ABI. */
#include <stdint.h>
#include <stdatomic.h>
#include <linux/sched.h>
#include <sys/syscall.h>
#include "adopt.h"
#include "../../compat-proxy/arch/x86_64/adopt.h"
#define N 16
static struct adopt_api *api;
static long raw(long n,long a,long b,long c,long d,long e,long f) {
    register long r10 __asm__("r10")=d;
    register long r8 __asm__("r8")=e;
    register long r9 __asm__("r9")=f;
    long out;__asm__ volatile("syscall":"=a"(out):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");return out;
}
static void message(const char *p) {unsigned n=0;while(p[n])n++;raw(1,1,(long)p,n,0,0,0);}
#define CHECK(x) do {if(!(x)) {message("FAIL raw: " #x "\n");raw(231,198,0,0,0,0,0);__builtin_trap();}} while(0)
struct raw_tcb {uintptr_t self,marker;};
struct child {
    struct raw_tcb tls;
    _Atomic int ptid,ctid,done;
    uintptr_t resident;
    void *mapping;
    int mode;
};
static struct raw_tcb main_tls;
static struct child children[N];
static _Atomic int go,signals;
static uintptr_t main_resident;
extern long raw_clone(unsigned long,void *,void *,void *,void *);
extern long raw_clone3(struct clone_args *,unsigned long);
extern long raw_vfork(struct child *);
extern void raw_restore(void),raw_unmap_exit(void *,unsigned long);
static uintptr_t marker(void) {uintptr_t p;__asm__ volatile("mov %%fs:8,%0":"=r"(p));return p;}
static long kernel_tid(void) {return raw(186,0,0,0,0,0,0);}
static void gate(int sig) {
    struct adopt_thread *t=api->current();CHECK(t && sig==10);
    unsigned char personality=atomic_exchange(&t->selector,0);
    uintptr_t prior=adopt_fs_read();adopt_fs_write(t->app_fs);
    CHECK(marker()==0xabcde);atomic_fetch_add(&signals,1);
    adopt_fs_write(prior);atomic_store(&t->selector,personality);
}
static uintptr_t identity(void) {
    struct adopt_thread *t=api->current();CHECK(t && atomic_load(&t->tid)==kernel_tid());
    atomic_store(&t->selector,0);uintptr_t fs=adopt_fs_read();
    adopt_fs_write(t->resident_fs);
    CHECK(raw(234,raw(39,0,0,0,0,0,0),kernel_tid(),10,0,0,0)==0);
    uintptr_t id;CHECK(api->identity(&id)==kernel_tid());
    adopt_fs_write(fs);atomic_store(&t->selector,1);
    CHECK(marker()==0xabcde);return id;
}
_Noreturn void raw_child(struct child *c) {
    if(c->mode==2) {
        char *args[]={"true",0};char *env[]={0};
        raw(59,(long)"/bin/true",(long)args,(long)env,0,0,0);
        raw(60,127,0,0,0,0,0);__builtin_trap();
    }
    CHECK(adopt_fs_read()==(uintptr_t)&c->tls && marker()==0xabcde);
    CHECK(atomic_load(&c->ctid)==kernel_tid());
    while(!atomic_load(&go)) raw(202,(long)&go,0,0,0,0,0);
    c->resident=identity();atomic_store(&c->done,1);
    if(c->mode==1) raw_unmap_exit(c->mapping,65536);
    raw(60,0,0,0,0,0,0);__builtin_trap();
}
static void *prepare(struct child *c) {
    long p=raw(9,0,65536,3,0x22,-1,0);CHECK(p>0);c->mapping=(void *)p;
    uintptr_t sp=((uintptr_t)p+65536-16)&~15UL;
    *(uintptr_t *)sp=(uintptr_t)c;
    *(uintptr_t *)(sp-8)=0x33445566;*(uintptr_t *)(sp-128)=0x55667711;
    c->tls.self=(uintptr_t)&c->tls;c->tls.marker=0xabcde;
    atomic_store(&c->ptid,-1);atomic_store(&c->ctid,-1);atomic_store(&c->done,0);
    return (void *)sp;
}
static void threads(int clone3) {
    while(api->live()!=1) raw(24,0,0,0,0,0,0);
    atomic_store(&go,0);
    for(unsigned i=0;i<N;i++) {
        struct child *c=&children[i];void *sp=prepare(c);c->mode=i&1;
        unsigned long flags=CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SYSVSEM|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID;
        struct clone_args a={.flags=flags,.stack=(uintptr_t)c->mapping,.stack_size=(uintptr_t)sp-(uintptr_t)c->mapping,.tls=(uintptr_t)&c->tls,.parent_tid=(uintptr_t)&c->ptid,.child_tid=(uintptr_t)&c->ctid};
        long tid=clone3?raw_clone3(&a,sizeof(a)):raw_clone(flags,sp,&c->ptid,&c->ctid,&c->tls);
        CHECK(tid>0 && atomic_load(&c->ptid)==tid && atomic_load(&c->ctid)==tid);
    }
    CHECK(api->live()==N+1);atomic_store(&go,1);raw(202,(long)&go,1,0x7fffffff,0,0,0);
    for(unsigned i=0;i<N;i++) {
        struct child *c=&children[i];int tid;
        while((tid=atomic_load(&c->ctid))) raw(202,(long)&c->ctid,0,tid,0,0,0);
        CHECK(atomic_load(&c->done) && c->resident!=main_resident);
        for(unsigned j=0;j<i;j++) CHECK(c->resident!=children[j].resident);
        if(!c->mode) CHECK(raw(11,(long)c->mapping,65536,0,0,0,0)==0);
    }
}
static void rejections(void) {
    struct clone_args a={.flags=CLONE_VM|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS|(1ULL<<63)};
    CHECK(raw(435,(long)&a,sizeof(a),0,0,0,0)==-38);
    CHECK(raw(56,a.flags,0,0,0,0,0)==-38);
    a.flags=CLONE_VM|CLONE_VFORK|(1ULL<<63);a.exit_signal=17;
    CHECK(raw(435,(long)&a,sizeof(a),0,0,0,0)==-22);
    CHECK(raw(56,a.flags|17,0,0,0,0,0)==-22);
    a.flags=CLONE_VM|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS;a.exit_signal=0;
    CHECK(raw(435,(long)&a,sizeof(a)+8,0,0,0,0)==-22);
    CHECK(raw(435,(long)&a,63,0,0,0,0)==-22);
    a.set_tid=1;CHECK(raw(435,(long)&a,sizeof(a),0,0,0,0)==-38);a.set_tid=0;
    a.cgroup=1;CHECK(raw(435,(long)&a,sizeof(a),0,0,0,0)==-38);a.cgroup=0;
    a.set_tid_size=1;CHECK(raw(435,(long)&a,sizeof(a),0,0,0,0)==-38);
    CHECK(api->live()==1);
}
void raw_main(uintptr_t *stack) {
    char **env=(char **)(stack+*stack+2);
    const char prefix[]="MATON_THREAD_ADOPT=";
    for(;*env;env++) {
        unsigned i=0;while(prefix[i] && prefix[i]==(*env)[i]) i++;
        if(!prefix[i]) {uintptr_t p=0;for(const char *s=*env+i;*s;s++) p=p*16+(*s>='a'?*s-'a'+10:*s-'0');api=(void *)p;}
    }
    CHECK(api && api->version==2 && api->lookup);
    main_tls.self=(uintptr_t)&main_tls;main_tls.marker=0xabcde;adopt_fs_write((uintptr_t)&main_tls);
    struct {void (*fn)(int);unsigned long flags;void (*restore)(void);unsigned long mask;} sa={gate,0x04000000,raw_restore,0};
    CHECK(raw(13,10,(long)&sa,0,8,0,0)==0);
    main_resident=identity();threads(0);threads(1);
    while(api->live()!=1) raw(24,0,0,0,0,0,0);
    rejections();
    struct child c={.mode=2};void *sp=prepare(&c);c.mode=2;
    long pid=raw_clone(CLONE_VM|CLONE_VFORK|17,sp,0,0,0);CHECK(pid>0);
    int status;CHECK(raw(61,pid,(long)&status,0,0,0,0)==pid && status==0);
    CHECK(raw(11,(long)c.mapping,65536,0,0,0,0)==0);
    pid=raw_vfork(&c);CHECK(pid>0);CHECK(raw(61,pid,(long)&status,0,0,0,0)==pid && status==0);
    CHECK(atomic_load(&signals)==33);
    message("PASS freestanding raw clone/clone3: TLS, SETTID/CLEARTID/wake, resident identities, registers/redzone, signals, unmap-self exit, rejection, vfork registers\n");
    raw(231,0,0,0,0,0,0);__builtin_trap();
}
