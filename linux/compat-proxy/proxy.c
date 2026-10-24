/* Linux layer v3. Resident state is inherited by fork; this is a spike,
 * NOT a security boundary. The original Android filter remains authoritative. */
#define _GNU_SOURCE
#define ARCH_WITH_CONTEXT
#include "proxy.h"
#include "state.h"
#include "vfs.h"
#include "net.h"
#include "links.h"
#include "sysvipc.h"
#define RESIDENT_ALLOC vfs_alloc
#define RESIDENT_FREE vfs_free
#include "resident-ranges.h"
#include <sys/time.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <linux/capability.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/limits.h>
#if ARCH_AUDIT == AUDIT_ARCH_I386
#include <asm/ldt.h>
#endif
#include <linux/prctl.h>
#include <stddef.h>
#include <fcntl.h>
#include <errno.h>
#include ARCH_POLICY_HEADER
#include "build/app-policy-dual.h"
#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif
#ifndef SYS_USER_DISPATCH
#define SYS_USER_DISPATCH 2
#endif
#define SIGSYS_BIT (UINT64_C(1) << (SIGSYS-1))
#define SA_RESTORER 0x04000000
#define NS_FLAGS (CLONE_NEWNS|CLONE_NEWCGROUP|CLONE_NEWUTS|CLONE_NEWIPC|CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWNET)
#ifdef HOST_COMMAND_TEST
static int hc_test_fd=-1;
#endif
static int proxy_count_fd=-1;
#include "resident-fds.h"
static int private_close_fd(int fd){
#ifdef HOST_COMMAND_TEST
    if(fd==hc_test_fd)return 1;
#endif
    /* --count spans resident execs even if payloads set CLOEXEC on it. */
    return fd==proxy_count_fd || vfs_private_fd(fd) || resident_fd_owned(fd);
}
static struct kernel_action chained;
static struct kernel_action chained_segv;
static void rewrite_segv_handler(int,siginfo_t *,void *);
int proxy_diagnostics_init(void) {
    if(proxy_diagnostic_fd!=2)return 0;
    /* F_DUPFD keeps the original sink through resident exec, without CLOEXEC.
     * The VFS private-fd guard protects it from the payload's fd sweeps. */
    long fd=RAW(PROXY_SC_fcntl,2,F_DUPFD,256,0,0,0);
    if(fd<0)return (int)fd;
    proxy_diagnostic_fd=(int)fd;return 0;
}
/* Resident exec must discard libc handlers: bionic debuggerd uses bionic
 * TLS, which is no longer valid after entering a glibc image. */
static void crash_hex(uint64_t value) {
    char out[18]="0x";static const char digits[]="0123456789abcdef";
    for(unsigned i=0;i<16;i++)out[2+i]=digits[(value>>(60-4*i))&15];
    RAW(PROXY_SC_write,proxy_diagnostic_fd,out,sizeof(out),0,0,0);
}
static void crash_text(const char *s){RAW(PROXY_SC_write,proxy_diagnostic_fd,s,p_len(s),0,0,0);}
static uintptr_t maps_hex(const char **p){return resident_maps_hex(p);}
struct image_map {uintptr_t lo,hi,offset;char *path;struct image_map *next;};
static struct image_map *image_maps;
#if ARCH_AUDIT == AUDIT_ARCH_I386
static void ldt_segv_handler(int,siginfo_t *,void *);
#endif
void proxy_image_map(uintptr_t lo,uintptr_t hi,uintptr_t offset,const char *path){
    struct image_map *m=vfs_palloc(sizeof(*m));if(!m)return;
    m->path=vfs_palloc(p_len(path)+1);if(!m->path){vfs_free(m);return;}
    p_copy(m->path,path,p_len(path)+1);m->lo=lo;m->hi=hi;m->offset=offset;
    m->next=image_maps;image_maps=m;
}
static void proxy_rewrite_dump(void *);
static void crash_report(int sig,siginfo_t *info,void *context) {
    ucontext_t *uc=context;uintptr_t pc=ARCH_CONTEXT(uc,ARCH_PC);
    crash_text("v3 fatal signal=");crash_hex(sig);
    crash_text(" pid=");crash_hex(RAW(PROXY_SC_getpid,0,0,0,0,0,0));
    crash_text(" tid=");crash_hex(RAW(PROXY_SC_gettid,0,0,0,0,0,0));
    crash_text(" code=");crash_hex(info->si_code);
    crash_text(" addr=");crash_hex((uintptr_t)info->si_addr);
    crash_text(" pc=");crash_hex(pc);crash_text("\n");
    for(struct image_map *m=image_maps;m;m=m->next)if(pc>=m->lo && pc<m->hi){
        crash_text("v3 fatal image=");crash_text(m->path);crash_text(" elf-offset=");crash_hex(pc-m->lo+m->offset);crash_text("\n");break;
    }
    proxy_rewrite_dump(context);
    /* Raw maps: bypass virtual proc paths; no libc/TLS/allocation in a crash. */
    long fd=RAW(PROXY_SC_openat,AT_FDCWD,"/proc/self/maps",O_RDONLY|O_CLOEXEC,0,0,0);
    if(fd>=0){char line[1024];size_t n=0;char c;
        while(RAW(PROXY_SC_read,fd,&c,1,0,0,0)==1){
            if(c!='\n'){if(n+1<sizeof(line))line[n++]=c;continue;}
            line[n]=0;const char *p=line;uintptr_t lo=maps_hex(&p),hi=0;
            if(*p=='-'){p++;hi=maps_hex(&p);}
            if(pc>=lo && pc<hi){crash_text("v3 fatal map=");crash_text(line);crash_text(" offset=");crash_hex(pc-lo);crash_text("\n");break;}n=0;
        }RAW(PROXY_SC_close,fd,0,0,0,0,0);
    }
    struct kernel_action action={0};RAW(PROXY_SC_rt_sigaction,sig,&action,0,8,0,0);
    proxy_sigmask_t mask=UINT64_C(1)<<(sig-1);RAW(PROXY_SC_rt_sigprocmask,SIG_UNBLOCK,&mask,0,8,0,0);
    RAW(PROXY_SC_tgkill,RAW(PROXY_SC_getpid,0,0,0,0,0,0),RAW(PROXY_SC_gettid,0,0,0,0,0,0),sig,0,0,0);
    RAW(PROXY_SC_exit_group,128+sig,0,0,0,0,0);
}
static void exec_signals(void) {
    for(int sig=1;sig<=64;sig++){
        if(sig==SIGKILL || sig==SIGSTOP || sig==SIGSYS)continue;
        struct kernel_action old={0},next={0};
        if(RAW(PROXY_SC_rt_sigaction,sig,0,&old,8,0,0)<0)continue;
        if(old.handler==1 && sig!=SIGSEGV)continue; /* synchronous SIGSEGV still needs proxy handling */
        if(sig==SIGSEGV)chained_segv=(struct kernel_action){0};
        if(sig==SIGSEGV || sig==SIGBUS || sig==SIGILL || sig==SIGFPE || sig==SIGABRT)
            next=(struct kernel_action){(uintptr_t)crash_report,SA_SIGINFO|SA_RESTORER|SA_ONSTACK,(uintptr_t)proxy_restore,0};
        if(sig==SIGSEGV)next=(struct kernel_action){(uintptr_t)rewrite_segv_handler,SA_SIGINFO|SA_RESTORER|SA_ONSTACK,(uintptr_t)proxy_restore,0};
#if ARCH_AUDIT == AUDIT_ARCH_I386
        if(sig==SIGSEGV)next=(struct kernel_action){(uintptr_t)ldt_segv_handler,SA_SIGINFO|SA_RESTORER|SA_ONSTACK,(uintptr_t)proxy_restore,0};
#endif
        RAW(PROXY_SC_rt_sigaction,sig,&next,0,8,0,0);
    }
}
/* Spike supports a single user-dispatch owner; configuration is per-thread. */
static volatile unsigned char *dispatch_selector;
static long dispatch_owner;
#if ARCH_AUDIT == AUDIT_ARCH_I386
struct tls_entry { struct user_desc desc; int real_entry; int valid; };
#define PROXY_TLS_ENTRIES 3
static int proxy_i386_tls_reservation_enabled;
struct ldt_entry { struct user_desc desc; unsigned char valid; };
static struct ldt_entry *ldt_entries;
static size_t ldt_capacity;
/* modify_ldt is process-wide. Serialize growth/writes with SIGSEGV readers. */
static volatile int ldt_lock;
static void ldt_lock_acquire(void) {
    while(__atomic_exchange_n(&ldt_lock,1,__ATOMIC_ACQUIRE))
        while(__atomic_load_n(&ldt_lock,__ATOMIC_RELAXED)) __asm__ volatile("pause");
}
static void ldt_lock_release(void) { __atomic_store_n(&ldt_lock,0,__ATOMIC_RELEASE); }
#define PROXY_LDT_CACHE_ENTRIES 3
struct ldt_cache_entry { unsigned selector; unsigned age; int real_entry; unsigned valid; };
#endif
struct thread_state {
    unsigned direct_depth;
    long tid; stack_t app_stack; void *signal_stack; size_t signal_stack_size;
#if ARCH_AUDIT == AUDIT_ARCH_I386
    struct tls_entry tls[PROXY_TLS_ENTRIES];
    unsigned reserved;
    unsigned reserved_claimed;
    int reserved_real;
    unsigned ldt_age;
    struct ldt_cache_entry ldt_cache[PROXY_LDT_CACHE_ENTRIES];
    unsigned logical_cs;
#endif
};
struct thread_node {struct thread_state state;struct thread_node *next;};
static struct thread_node *threads;
/* The static proxy runs before it can rely on the payload's TLS layout. Cache
 * state by the current stack page instead; stacks are per-thread, and an
 * ambiguous page falls back to gettid rather than guessing ownership. */
struct thread_stack_cache {uintptr_t page;struct thread_state *state;struct thread_stack_cache *next;};
static struct thread_stack_cache *thread_stack_caches;
static void thread_stack_cache_add(uintptr_t page,struct thread_state *state){
    struct thread_stack_cache *fresh=NULL;
    for(;;){
        struct thread_stack_cache *head=__atomic_load_n(&thread_stack_caches,__ATOMIC_ACQUIRE);
        for(struct thread_stack_cache *c=head;c;c=c->next){
            if(c->page==page){if(c->state!=state)__atomic_store_n(&c->state,NULL,__ATOMIC_RELEASE);vfs_free(fresh);return;}
        }
        if(!fresh){fresh=vfs_palloc(sizeof(*fresh));if(!fresh)return;}
        fresh->page=page;fresh->state=state;fresh->next=head;
        if(__atomic_compare_exchange_n(&thread_stack_caches,&head,fresh,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED))return;
    }
}
static void thread_state_cache_forget(struct thread_state *state){
    for(struct thread_stack_cache *c=__atomic_load_n(&thread_stack_caches,__ATOMIC_ACQUIRE);c;c=c->next)
        if(__atomic_load_n(&c->state,__ATOMIC_ACQUIRE)==state)__atomic_store_n(&c->state,NULL,__ATOMIC_RELEASE);
}
void proxy_thread_state_invalidate(void){uintptr_t stack=(uintptr_t)&stack,page=stack&~(uintptr_t)4095;
    for(struct thread_stack_cache *c=__atomic_load_n(&thread_stack_caches,__ATOMIC_ACQUIRE);c;c=c->next)if(c->page==page)__atomic_store_n(&c->state,NULL,__ATOMIC_RELEASE);}
/* Allocation hot paths can reuse an already proven stack-page owner without
 * allocating state or issuing a syscall. Unknown/ambiguous pages return zero. */
long proxy_stack_tid(uintptr_t sp){
    uintptr_t page=sp&~(uintptr_t)4095;
    for(struct thread_stack_cache *c=__atomic_load_n(&thread_stack_caches,__ATOMIC_ACQUIRE);c;c=c->next)
        if(c->page==page){struct thread_state *state=__atomic_load_n(&c->state,__ATOMIC_ACQUIRE);return state?state->tid:0;}
    return 0;
}
static void thread_state_cache_invalidate_all(void){
    for(struct thread_stack_cache *c=__atomic_load_n(&thread_stack_caches,__ATOMIC_ACQUIRE);c;c=c->next)
        __atomic_store_n(&c->state,NULL,__ATOMIC_RELEASE);
}
#if ARCH_AUDIT == AUDIT_ARCH_I386 && defined(MATON_GPU_BRIDGE)
static long threads_pid;
#endif
static struct thread_state *thread_state(void) {
    uintptr_t stack=(uintptr_t)&stack,page=stack&~(uintptr_t)4095;
    for(struct thread_stack_cache *c=__atomic_load_n(&thread_stack_caches,__ATOMIC_ACQUIRE);c;c=c->next)
        if(c->page==page){struct thread_state *cached=__atomic_load_n(&c->state,__ATOMIC_ACQUIRE);if(cached)return cached;break;}
    long tid=RAW(PROXY_SC_gettid,0,0,0,0,0,0);
    for(struct thread_node *p=threads;p;p=p->next) if(p->state.tid==tid){thread_stack_cache_add(page,&p->state);return &p->state;}
#if ARCH_AUDIT == AUDIT_ARCH_I386 && defined(MATON_GPU_BRIDGE)
    long pid=RAW(PROXY_SC_getpid,0,0,0,0,0,0);
    if(threads_pid && pid!=threads_pid) {
        unsigned short gs;__asm__ volatile("movw %%gs,%0":"=rm"(gs));
        uintptr_t base=0;
        if(gs && !(gs&4)) {
            /* Both glibc and bionic x86 TLS use GS:0 as the self pointer.
             * get_thread_area is excluded by Android's inherited filter. */
            __asm__ volatile("movl %%gs:0,%0" : "=r"(base));
            for(struct thread_node *p=threads;p;p=p->next)
                for(unsigned i=0;i<PROXY_TLS_ENTRIES;i++) {
                    struct tls_entry *entry=&p->state.tls[i];
                    if(entry->valid && entry->real_entry==(int)(gs>>3) &&
                       entry->desc.base_addr==base) {
                        /* Fork copied this thread's GDT, virtual slots and
                         * altstack. Retain the reservation rather than trying
                         * to allocate a fourth hardware TLS descriptor. */
                        p->state.tid=tid;threads_pid=pid;thread_stack_cache_add(page,&p->state);return &p->state;
                    }
                }
        }
    }
    threads_pid=pid;
#endif
    struct thread_node *p=vfs_palloc(sizeof(*p));if(!p)return NULL;
    p->state.tid=tid;p->state.app_stack.ss_flags=SS_DISABLE;
    stack_t current;
    if(!RAW(PROXY_SC_sigaltstack,0,&current,0,0,0,0)) {
        p->state.signal_stack=current.ss_sp;p->state.signal_stack_size=current.ss_size;
    }
    if(!(current.ss_flags&SS_ONSTACK)) {
        long area=RAW(PROXY_SC_mmap,0,1<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if((unsigned long)area>=(unsigned long)-4095){vfs_free(p);return NULL;}
        stack_t own={.ss_sp=(void *)area,.ss_size=1<<20};
        if(RAW(PROXY_SC_sigaltstack,&own,0,0,0,0,0)){RAW(PROXY_SC_munmap,area,1<<20,0,0,0,0);vfs_free(p);return NULL;}
        p->state.signal_stack=(void *)area;p->state.signal_stack_size=1<<20;
    }
    do {p->next=__atomic_load_n(&threads,__ATOMIC_ACQUIRE);}
    while(!__atomic_compare_exchange_n(&threads,&p->next,p,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED));
    thread_stack_cache_add(page,&p->state);return &p->state;
}
#if ARCH_AUDIT == AUDIT_ARCH_I386
static int tls_slot_for_real(struct thread_state *thread, int real_entry) {
    for (int i=0;i<PROXY_TLS_ENTRIES;i++)
        if(thread->tls[i].valid && thread->tls[i].real_entry==real_entry)return i;
    if(thread->reserved && thread->reserved_real==real_entry)return PROXY_TLS_ENTRIES-1;
    return -1;
}
static int tls_reserve(struct thread_state *thread) {
    if(thread->reserved)return 0;
    struct user_desc d={.entry_number=(unsigned)-1,.seg_32bit=1,
        .contents=0,.read_exec_only=1,.seg_not_present=0,.useable=1};
    long r=RAW(PROXY_SC_set_thread_area,(long)&d,0,0,0,0,0);
    if(r<0)return (int)r;
    thread->reserved_real=(int)d.entry_number;thread->reserved=1;return 0;
}
void proxy_i386_tls_reservation(int enabled) {
    proxy_i386_tls_reservation_enabled=enabled!=0;
}
int proxy_i386_tls_prepare_payload(void) {
    if(!proxy_i386_tls_reservation_enabled)return 0;
    struct thread_state *thread=thread_state();
    return thread?tls_reserve(thread):-ENOMEM;
}
static int tls_install_reserved(struct thread_state *thread,const struct user_desc *desc) {
    struct user_desc install=*desc;
    install.entry_number=thread->reserved_real;
    long r=RAW(PROXY_SC_set_thread_area,(long)&install,0,0,0,0,0);
    return r<0?(int)r:0;
}
static int tls_restore_reserved(struct thread_state *thread) {
    if(thread->tls[PROXY_TLS_ENTRIES-1].valid)
        return tls_install_reserved(thread,&thread->tls[PROXY_TLS_ENTRIES-1].desc);
    const struct user_desc placeholder={.entry_number=0,.seg_32bit=1,
        .contents=0,.read_exec_only=1,.seg_not_present=0,.useable=1};
    return tls_install_reserved(thread,&placeholder);
}
static long tls_set(struct thread_state *thread, uintptr_t user) {
    struct user_desc desc;
    if(!user || proxy_copy(&desc,(const void *)user,sizeof(desc)))return -EFAULT;
    int slot=-1;
    if(desc.entry_number==UINT32_MAX) {
        for(int i=0;i<PROXY_TLS_ENTRIES;i++)
            if(!thread->tls[i].valid){slot=i;break;}
        if(slot<0)return -ESRCH;
    } else if(desc.entry_number>=PROXY_TLS_ENTRIES) {
        /* Applications use the real index returned by an earlier allocation. */
        if(desc.entry_number>INT32_MAX)return -EINVAL;
        slot=tls_slot_for_real(thread,(int)desc.entry_number);
        if(slot<0)return -ESRCH;
    } else slot=(int)desc.entry_number;
    /* Reservation is eager, before payload code runs, when enabled. */
    if(slot==PROXY_TLS_ENTRIES-1){int r=tls_reserve(thread);if(r)return r;}
    struct user_desc kernel_desc=desc;
    kernel_desc.entry_number=slot==PROXY_TLS_ENTRIES-1 && thread->reserved
        ? thread->reserved_real : (thread->tls[slot].valid?thread->tls[slot].real_entry:-1);
    long result=0;
    if(slot==PROXY_TLS_ENTRIES-1 && thread->reserved_claimed) {
        /* Keep the payload's descriptor current while bionic owns the real slot. */
        desc.entry_number=thread->reserved_real;
    } else {
        result=RAW(PROXY_SC_set_thread_area,(long)&kernel_desc,0,0,0,0,0);
        if(result<0)return result;
        desc.entry_number=kernel_desc.entry_number;
    }
    if(proxy_copy((void *)user,&desc,sizeof(desc)))return -EFAULT;
    thread->tls[slot].desc=desc;
    thread->tls[slot].real_entry=kernel_desc.entry_number;
    thread->tls[slot].valid=1;
    return 0;
}
/* Bionic adoption temporarily owns the reserved entry. Release restores the
 * payload's third virtual slot, or the inert placeholder if it is unused. */
int proxy_tls_claim_reserved(const struct user_desc *owner,unsigned *selector) {
    struct thread_state *thread=thread_state();
    if(!thread || !selector || !owner || thread->reserved_claimed)return -1;
    int r=tls_reserve(thread);if(r)return r;
    r=tls_install_reserved(thread,owner);if(r)return r;
    thread->reserved_claimed=1;
    *selector=((unsigned)thread->reserved_real<<3)|3u;
    return 0;
}
int proxy_tls_release_reserved(void) {
    struct thread_state *thread=thread_state();
    if(!thread || !thread->reserved || !thread->reserved_claimed)return -1;
    int r=tls_restore_reserved(thread);if(r)return r;
    thread->reserved_claimed=0;
    return 0;
}
static long tls_get(struct thread_state *thread, uintptr_t user) {
    struct user_desc desc;
    if(!user || proxy_copy(&desc,(const void *)user,sizeof(desc)))return -EFAULT;
    int slot=desc.entry_number<PROXY_TLS_ENTRIES?desc.entry_number:
             tls_slot_for_real(thread,desc.entry_number);
    if(slot<0 || slot>=PROXY_TLS_ENTRIES || !thread->tls[slot].valid)return -ESRCH;
    desc=thread->tls[slot].desc;
    desc.entry_number=thread->tls[slot].real_entry;
    return proxy_copy((void *)user,&desc,sizeof(desc))?-EFAULT:0;
}
#ifdef MATON_GPU_BRIDGE
int proxy_i386_tls_current(struct user_desc *desc) {
    struct thread_state *thread=thread_state();
    return thread?(int)tls_get(thread,(uintptr_t)desc):-ENOMEM;
}
#endif
static int ldt_grow(size_t need) {
    if(need>8192)return -EINVAL;
    if(need<=ldt_capacity)return 0;
    size_t cap=ldt_capacity?ldt_capacity:16;
    while(cap<need){if(cap>4096){cap=8192;break;}cap*=2;}
    if(cap>SIZE_MAX/sizeof(*ldt_entries))return -ENOMEM;
    struct ldt_entry *next=vfs_alloc(cap*sizeof(*next));if(!next)return -ENOMEM;
    for(size_t i=0;i<cap;i++)next[i]=(struct ldt_entry){0};
    for(size_t i=0;i<ldt_capacity;i++)next[i]=ldt_entries[i];
    if(ldt_entries)vfs_free(ldt_entries);
    ldt_entries=next;ldt_capacity=cap;return 0;
}
static long ldt_call_locked(unsigned func, uintptr_t ptr, size_t bytes) {
    if(func==2)return 0; /* read_default: no default LDT entries */
    if(func==1 || func==0x11) {
        struct user_desc d;
        if(!ptr || proxy_copy(&d,(void *)ptr,sizeof(d)))return -EFAULT;
        /* Flat 32-bit code is used only by emulated far control transfers;
         * its limit is intentionally not enforced (unlike hardware). */
        int code=d.contents==2;
        if(!d.seg_32bit || (!code && d.limit>0xfffffu) || (d.contents>1 && !code) || d.read_exec_only>1 ||
           d.limit_in_pages>1 || d.seg_not_present>1 || d.useable>1)return -EINVAL;
        if(code && (d.base_addr!=0 || d.seg_not_present))return -EINVAL;
        if(d.entry_number>=8192)return -EINVAL;
        int r=ldt_grow((size_t)d.entry_number+1);if(r)return r;
        ldt_entries[d.entry_number].desc=d;
        ldt_entries[d.entry_number].valid=!(d.seg_not_present ||
            (d.contents!=2 && d.base_addr==0 && d.limit==0 && d.read_exec_only));
        return 0;
    }
    if(func==0) {
        if(!ptr && bytes)return -EFAULT;
        size_t copied=0;
        for(size_t i=0;i<ldt_capacity && copied+8<=bytes;i++) {
            uint64_t raw=0;
            if(ldt_entries[i].valid) {
                struct user_desc d=ldt_entries[i].desc;
                raw|=(uint64_t)(d.limit&0xffffu);
                raw|=(uint64_t)(d.base_addr&0xffffu)<<16;
                raw|=(uint64_t)((d.base_addr>>16)&0xffu)<<32;
                raw|=(uint64_t)(d.seg_32bit&1u)<<54;
                raw|=(uint64_t)(d.read_exec_only?0:1)<<41;
                raw|=(uint64_t)(d.limit&0xf0000u)<<32;
                raw|=(uint64_t)((d.base_addr>>24)&0xffu)<<56;
                raw|=(uint64_t)(d.useable&1u)<<52;
                raw|=(uint64_t)(d.limit_in_pages?1u:0u)<<55;
                raw|=UINT64_C(1)<<47; /* present */
                raw|=UINT64_C(1)<<44; /* code/data */
                raw|=UINT64_C(3)<<45; /* user privilege */
                raw|=UINT64_C(1)<<40; /* accessed */
                raw|=(uint64_t)(d.contents&1u)<<42; /* expand-down data */
                raw|=(uint64_t)(d.read_exec_only?0:1)<<41; /* writable data / readable code */
                if(d.contents==2)raw|=UINT64_C(1)<<43; /* executable code */
            }
            if(proxy_copy((void *)(ptr+copied),&raw,8))return copied?(long)copied:-EFAULT;
            copied+=8;
        }
        return (long)copied;
    }
    return -EINVAL;
}
static long ldt_call(unsigned func,uintptr_t ptr,size_t bytes) {
    ldt_lock_acquire();long result=ldt_call_locked(func,ptr,bytes);ldt_lock_release();return result;
}
static uint32_t *i386_reg(ucontext_t *uc,unsigned r) {
    static const int indices[8]={REG_EAX,REG_ECX,REG_EDX,REG_EBX,REG_ESP,REG_EBP,REG_ESI,REG_EDI};
    return (uint32_t *)&uc->uc_mcontext.gregs[indices[r&7]];
}
static int i386_ea(ucontext_t *uc,const unsigned char *insn,size_t *len,
                   unsigned modrm,uint32_t *address) {
    unsigned mod=modrm>>6,rm=modrm&7;uint32_t base=0,index=0;int have_base=1;
    if(mod==3)return -1;
    if(rm==4) {
        unsigned char sib=insn[(*len)++];unsigned scale=sib>>6,ir=(sib>>3)&7,br=sib&7;
        if(ir!=4)index=*i386_reg(uc,ir)<<scale;
        if(mod==0 && br==5)have_base=0;else base=*i386_reg(uc,br);
    } else if(mod==0 && rm==5)have_base=0;
    else base=*i386_reg(uc,rm);
    if(mod==1){base+=(int8_t)insn[(*len)++];}
    else if(mod==2 || (mod==0&&!have_base)){
        uint32_t disp; p_copy(&disp,insn+*len,4);*len+=4;base+=disp;
    }
    *address=base+index;return 0;
}
static unsigned i386_segment_reg(ucontext_t *uc,int reg) {
    static const int indices[6]={REG_ES,REG_CS,REG_SS,REG_DS,REG_FS,REG_GS};
    return (unsigned)uc->uc_mcontext.gregs[indices[reg]]&0xffffu;
}
static int i386_cache_held(ucontext_t *uc,const struct ldt_cache_entry *cache) {
    unsigned virtual_index=cache->selector;
    unsigned real_index=(unsigned)cache->real_entry;
    for(int r=0;r<6;r++) {
        unsigned selector=i386_segment_reg(uc,r);
        if((selector&4) && (selector>>3)==virtual_index)return 1;
        if(!(selector&4) && (selector>>3)==real_index)return 1;
    }
    return 0;
}
static int ldt_cache_install(struct thread_state *thread,ucontext_t *uc,
                             unsigned selector,const struct user_desc *source,
                             int *real_entry) {
    unsigned index=selector>>3;
    for(int i=0;i<PROXY_LDT_CACHE_ENTRIES;i++) {
        struct ldt_cache_entry *c=&thread->ldt_cache[i];
        if(c->valid && c->selector==index) {
            c->age=++thread->ldt_age;*real_entry=c->real_entry;return 0;
        }
    }
    int slot=-1;
    for(int i=0;i<PROXY_LDT_CACHE_ENTRIES;i++) if(!thread->ldt_cache[i].valid){slot=i;break;}
    if(slot<0) {
        unsigned oldest=UINT32_MAX;
        for(int i=0;i<PROXY_LDT_CACHE_ENTRIES;i++) {
            struct ldt_cache_entry *c=&thread->ldt_cache[i];
            if(!i386_cache_held(uc,c) && c->age<oldest) {oldest=c->age;slot=i;}
        }
        if(slot<0)return -1;
    }
    struct ldt_cache_entry *c=&thread->ldt_cache[slot];
    struct user_desc d=*source;
    d.entry_number=c->valid?(unsigned)c->real_entry:UINT32_MAX;
    long r=RAW(PROXY_SC_set_thread_area,(long)&d,0,0,0,0,0);
    if(r<0 && !c->valid) {
        /* Other TLS users may already occupy the remaining GDT slots. Reuse
         * an unheld cache entry before reporting descriptor exhaustion. */
        unsigned oldest=UINT32_MAX;int victim=-1;
        for(int i=0;i<PROXY_LDT_CACHE_ENTRIES;i++) {
            struct ldt_cache_entry *candidate=&thread->ldt_cache[i];
            if(candidate->valid && !i386_cache_held(uc,candidate) && candidate->age<oldest) {
                oldest=candidate->age;victim=i;
            }
        }
        if(victim<0)return -1;
        slot=victim;c=&thread->ldt_cache[slot];d=*source;
        d.entry_number=(unsigned)c->real_entry;
        r=RAW(PROXY_SC_set_thread_area,(long)&d,0,0,0,0,0);
    }
    if(r<0)return -1;
    c->selector=index;c->real_entry=(int)d.entry_number;c->age=++thread->ldt_age;c->valid=1;
    *real_entry=c->real_entry;return 0;
}
static void dispatch_segv_app(int sig,siginfo_t *info,void *ctx) {
    struct kernel_action action=chained_segv;
    if(action.handler==1 && info->si_code<=0)return;
    if(action.handler<=1) {
        proxy_log("fatal-segv",info->si_code,(long)info->si_addr);
        proxy_log("fatal-segv-pc",0,(long)ARCH_CONTEXT((ucontext_t *)ctx,ARCH_PC));
        RAW(PROXY_SC_exit_group,128+sig,0,0,0,0,0);
        return;
    }
    if(action.flags&SA_RESETHAND)chained_segv.handler=0;
    proxy_sigmask_t old=0,mask=action.mask&~SIGSYS_BIT;
    RAW(PROXY_SC_rt_sigprocmask,SIG_BLOCK,&mask,&old,8,0,0);
    if(action.flags&SA_SIGINFO)((void (*)(int,siginfo_t *,void *))action.handler)(sig,info,ctx);
    else ((void (*)(int))action.handler)(sig);
    old&=~SIGSYS_BIT;RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&old,0,8,0,0);
}
static int ldt_is_flat_code(unsigned selector) {
    if(!(selector&4) || (selector&3)!=3)return 0;
    unsigned index=selector>>3;int valid=0;
    ldt_lock_acquire();
    if(index<ldt_capacity && ldt_entries[index].valid) {
        const struct user_desc *d=&ldt_entries[index].desc;
        valid=d->contents==2 && d->seg_32bit && d->base_addr==0 && !d->seg_not_present;
    }
    ldt_lock_release();return valid;
}
static int i386_code_selector(unsigned selector) {
    return selector==0x23 || ldt_is_flat_code(selector);
}
static int i386_stack_read(uint32_t sp,void *value,size_t size) {
    return proxy_copy(value,(const void *)(uintptr_t)sp,size)==0;
}
static int i386_stack_write(uint32_t sp,const void *value,size_t size) {
    return proxy_copy((void *)(uintptr_t)sp,value,size)==0;
}
static void ldt_segv_handler(int sig,siginfo_t *info,void *opaque) {
    ucontext_t *uc=opaque;unsigned char op[16];size_t n=0,ilen=0;uint32_t selector=0,address=0;
    int target=-1,load_offset=-1;uint32_t offset=0;int pop=0;
    uintptr_t pc=(uintptr_t)uc->uc_mcontext.gregs[REG_EIP];
    int transfer=0,transfer_kind=0; /* 1 jump, 2 call, 3 retf, 4 iret */
    unsigned transfer_selector=0;uint32_t transfer_offset=0;unsigned ret_adjust=0;
    if(info->si_code==SI_KERNEL && pc && !proxy_copy(op,(void *)pc,sizeof(op))) {
        if(op[0]==0x9a) { /* lcall ptr16:32 */
            p_copy(&transfer_offset,op+1,4);p_copy(&transfer_selector,op+5,2);ilen=7;
            transfer=1;transfer_kind=2;
        } else if(op[0]==0xea) { /* ljmp ptr16:32 */
            p_copy(&transfer_offset,op+1,4);p_copy(&transfer_selector,op+5,2);ilen=7;
            transfer=1;transfer_kind=1;
        } else if(op[0]==0xff) {
            unsigned m=op[1],ext=(m>>3)&7;size_t elen=2;
            if((ext==3 || ext==5) && !i386_ea(uc,op,&elen,m,&address) &&
               !proxy_copy(&transfer_offset,(void *)(uintptr_t)address,4) &&
               !proxy_copy(&transfer_selector,(void *)(uintptr_t)(address+4),2)) {
                ilen=elen;transfer=1;transfer_kind=ext==3?2:1;
            }
        } else if(op[0]==0xcb || op[0]==0xca) { /* retf / retf imm16 */
            transfer=1;transfer_kind=3;ilen=op[0]==0xcb?1:3;
            if(op[0]==0xca){unsigned short adjust;p_copy(&adjust,op+1,2);ret_adjust=adjust;}
        } else if(op[0]==0xcf) { /* iret */
            transfer=1;transfer_kind=4;ilen=1;
        } else if(op[0]==0x8e) {
            unsigned m=op[1],seg=(m>>3)&7; n=2;
            if(seg==0 || seg==2 || seg==3 || seg==4 || seg==5) {
                if((m>>6)==3){selector=*i386_reg(uc,m&7)&0xffff;ilen=2;}
                else if(!i386_ea(uc,op,&n,m,&address) && !proxy_copy(&selector,(void *)(uintptr_t)address,2))ilen=n;
                else ilen=0;
                target=(int)seg;
            } else ilen=0;
        } else if(op[0]==0x07 || op[0]==0x17 || op[0]==0x1f) {
            uint32_t sp=(uint32_t)uc->uc_mcontext.gregs[REG_ESP];
            if(!proxy_copy(&selector,(void *)(uintptr_t)sp,2)) {
                target=op[0]==0x07?0:op[0]==0x17?2:3;pop=1;ilen=1;
            }
        } else if(op[0]==0xc4 || op[0]==0xc5) {
            unsigned m=op[1],dest=(m>>3)&7;size_t elen=2;
            if(!i386_ea(uc,op,&elen,m,&address) &&
               !proxy_copy(&offset,(void *)(uintptr_t)address,4) &&
               !proxy_copy(&selector,(void *)(uintptr_t)(address+4),2)) {
                load_offset=(int)dest;target=op[0]==0xc4?0:3;ilen=elen;
            }
        } else if(op[0]==0x0f && (op[1]==0xa1 || op[1]==0xa9)) {
            uint32_t sp=(uint32_t)uc->uc_mcontext.gregs[REG_ESP];
            if(!proxy_copy(&selector,(void *)(uintptr_t)sp,2)){
                target=op[1]==0xa1?4:5;pop=1;ilen=2;
            }
        } else if(op[0]==0x0f && (op[1]==0xb4 || op[1]==0xb5)) {
            unsigned m=op[2],dest=(m>>3)&7;size_t elen=3;uint32_t off;
            if(!i386_ea(uc,op,&elen,m,&address) && !proxy_copy(&off,(void *)(uintptr_t)address,4) &&
               !proxy_copy(&selector,(void *)(uintptr_t)(address+4),2)) {
                offset=off;load_offset=(int)dest;target=op[1]==0xb4?4:5;ilen=elen;
            }
        } else if(op[0]==0x0f && op[1]==0xb2) {
            unsigned m=op[2];size_t elen=3;
            if(!i386_ea(uc,op,&elen,m,&address) &&
               !proxy_copy(&offset,(void *)(uintptr_t)address,4) &&
               !proxy_copy(&selector,(void *)(uintptr_t)(address+4),2)) {
                load_offset=(int)((m>>3)&7);target=2;ilen=elen;
            }
        }
    }
    if(transfer && ilen) {
        uint32_t sp=(uint32_t)uc->uc_mcontext.gregs[REG_ESP];
        struct thread_state *thread=thread_state();
        unsigned current_cs=thread && thread->logical_cs?thread->logical_cs:0x23;
        int valid=0;
        if(transfer_kind==1 || transfer_kind==2) {
            valid=ldt_is_flat_code(transfer_selector);
            if(valid && transfer_kind==2) {
                uint32_t return_eip=(uint32_t)(pc+ilen),cs_word=current_cs;
                uint32_t next_sp=sp-4;
                if(!i386_stack_write(next_sp,&cs_word,4))valid=0;
                else { next_sp-=4;if(!i386_stack_write(next_sp,&return_eip,4))valid=0;
                    else uc->uc_mcontext.gregs[REG_ESP]=(greg_t)next_sp; }
            }
            if(valid && thread)thread->logical_cs=transfer_selector;
            if(valid)uc->uc_mcontext.gregs[REG_EIP]=(greg_t)transfer_offset;
        } else {
            uint32_t return_eip=0,cs_word=0,flags=0;
            unsigned count=transfer_kind==4?3:2;
            if(i386_stack_read(sp,&return_eip,4) &&
               i386_stack_read(sp+4,&cs_word,4) &&
               (count==2 || i386_stack_read(sp+8,&flags,4)) &&
               i386_code_selector(cs_word&0xffffu)) {
                transfer_selector=cs_word&0xffffu;valid=1;
                uc->uc_mcontext.gregs[REG_EIP]=(greg_t)return_eip;
                uc->uc_mcontext.gregs[REG_ESP]=(greg_t)(sp+(count==3?12:8)+ret_adjust);
                if(count==3)uc->uc_mcontext.gregs[REG_EFL]=(greg_t)flags;
                if(thread)thread->logical_cs=transfer_selector;
            }
        }
        if(valid)return;
    }
    struct user_desc ldt_desc;int ldt_valid=0;
    if(ilen && (selector&4) && target>=0) {
        unsigned index=selector>>3;
        ldt_lock_acquire();
        if(index<ldt_capacity && ldt_entries[index].valid) {ldt_desc=ldt_entries[index].desc;ldt_valid=1;}
        ldt_lock_release();
    }
    if(ldt_valid) {
        struct thread_state *thread=thread_state();
        int entry=-1;
        if(thread && !ldt_cache_install(thread,uc,selector,&ldt_desc,&entry)) {
            static const int indices[6]={REG_ES,REG_CS,REG_SS,REG_DS,REG_FS,REG_GS};
            unsigned real=((unsigned)entry<<3)|3u;
            if(load_offset>=0)*i386_reg(uc,(unsigned)load_offset)=offset;
            uc->uc_mcontext.gregs[indices[target]]=real;
            if(pop)uc->uc_mcontext.gregs[REG_ESP]+=4;
            uc->uc_mcontext.gregs[REG_EIP]+=ilen;return;
        }
    }
    dispatch_segv_app(sig,info,opaque);
}
#ifdef MATON_GPU_BRIDGE
/* A fresh bionic pthread becomes an adopted payload thread. Free its native
 * TLS descriptor before installing the opaque CLONE_SETTLS descriptor, then
 * reserve the third virtual slot. No libc may run with GS cleared. */
int proxy_i386_tls_enter(unsigned native_gs) {
    struct thread_state *thread=thread_state();
    if(!thread || !thread->reserved)return -ESRCH;
    __asm__ volatile("xor %%eax,%%eax; mov %%ax,%%gs" ::: "eax","memory");
    /* The linker may retain its temporary startup TLS descriptor as well as
     * libc's current one. Neither is needed while payload code runs: bionic
     * enters through the reserved descriptor using the saved current base. */
    unsigned native_entry=native_gs>>3;
    for(unsigned entry=native_entry-2;entry<=native_entry+2;entry++) {
        if(entry==(unsigned)thread->reserved_real)continue;
        struct user_desc empty={.entry_number=entry,.read_exec_only=1,.seg_not_present=1};
        long result=RAW(PROXY_SC_set_thread_area,&empty,0,0,0,0,0);
        /* Entries outside the kernel TLS GDT range are simply invalid. */
        if(result && result!=-EINVAL)return result;
    }
    return 0;
}
int proxy_i386_tls_start(const struct user_desc *app,unsigned native_gs,long parent_tid) {
    struct thread_state *thread=thread_state(),*parent=NULL;
    if(!thread)return -ENOMEM;
    for(struct thread_node *p=threads;p;p=p->next)
        if(p->state.tid==parent_tid) {parent=&p->state;break;}
    if(!parent || !parent->reserved)return -ESRCH;
    /* The creator stays parked until this copy is complete. These are our
     * virtual kernel descriptors, including the app's third slot while the
     * real slot was temporarily owned by the parent's bionic scope. */
    p_copy(thread->tls,parent->tls,sizeof(thread->tls));
    thread->reserved=1;thread->reserved_real=parent->reserved_real;
    thread->reserved_claimed=0;
    int slot=tls_slot_for_real(thread,(int)app->entry_number);
    if(slot<0)return -ESRCH;
    thread->tls[slot].desc=*app;thread->tls[slot].real_entry=app->entry_number;
    thread->tls[slot].valid=1;
    __asm__ volatile("xor %%eax,%%eax; mov %%ax,%%gs" ::: "eax","memory");
    struct user_desc empty={.entry_number=native_gs>>3,
                           .read_exec_only=1,.seg_not_present=1};
    long result=RAW(PROXY_SC_set_thread_area,&empty,0,0,0,0,0);
    if(result)return result;
    for(unsigned i=0;i<PROXY_TLS_ENTRIES;i++)if(thread->tls[i].valid) {
        struct user_desc install=thread->tls[i].desc;
        result=RAW(PROXY_SC_set_thread_area,&install,0,0,0,0,0);
        if(result)return result;
    }
    return tls_restore_reserved(thread);
}

void proxy_i386_thread_exit(void) {
    struct thread_state *thread=thread_state();
    if(!thread)return;
    thread_state_cache_forget(thread);
    vfs_trap_thread_exit(thread->signal_stack,thread->signal_stack_size);
    stack_t disabled={.ss_flags=SS_DISABLE};
    RAW(PROXY_SC_sigaltstack,&disabled,0,0,0,0,0);
    RAW(PROXY_SC_munmap,thread->signal_stack,thread->signal_stack_size,0,0,0,0);
    thread->tid=0;
}

#endif
/* Called in the child clone continuation before payload code runs. The kernel
 * installed glibc's CLONE_SETTLS descriptor; reserve the virtual third slot
 * independently before returning to payload code. */
void proxy_clone_tls_child(unsigned flags, const struct user_desc *user_desc) {
    if(!(flags&CLONE_SETTLS) || !user_desc)return;
    struct thread_state *thread=thread_state();
    if(!thread)return;
    if(proxy_i386_tls_reservation_enabled)tls_reserve(thread);
}
#endif
#if ARCH_AUDIT != AUDIT_ARCH_I386
void proxy_i386_tls_reservation(int enabled) {(void)enabled;}
int proxy_i386_tls_prepare_payload(void) {return 0;}
#endif
const char *proxy_tmpdir="/tmp";
/* --trace: log to the original diagnostic sink, independently of app stdio. */
int proxy_trace;
static volatile uint64_t proxy_counts[PROXY_SC_count];
static volatile uint64_t proxy_syscall_ns[PROXY_SC_count];
static volatile uint64_t proxy_handler_ns,proxy_vfs_ns;
uint64_t proxy_count_now(void){
#ifdef PROXY_PROFILE
    return 0; /* Keep --count's two clock syscalls/trap out of TSC profiling. */
#else
    if(proxy_count_fd<0)return 0;
    struct {long sec,nsec;} ts={0};
    if(RAW(PROXY_SC_clock_gettime,1,&ts,0,0,0,0)<0)return 0;
    return (uint64_t)ts.sec*1000000000ULL+(uint64_t)ts.nsec;
#endif
}
void proxy_count_init(const char *path){if(!path || !*path)return;long fd=RAW(PROXY_SC_openat,AT_FDCWD,path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600,0,0);if(fd>=0){proxy_count_fd=(int)fd;proxy_raw_count_enable();}}
void proxy_count_resolution(uint64_t ns){__atomic_fetch_add(&proxy_vfs_ns,ns,__ATOMIC_RELAXED);}
static void count_u64(char *b,size_t *n,uint64_t v){char d[24];size_t k=0;do{d[k++]=(char)('0'+v%10);v/=10;}while(v);while(k)b[(*n)++]=d[--k];}
void proxy_count_dump(void){if(proxy_count_fd<0)return;char b[PROXY_SC_count*64+128];size_t n=0;
    uint64_t handler=__atomic_exchange_n(&proxy_handler_ns,0,__ATOMIC_RELAXED);
    uint64_t vfs=__atomic_exchange_n(&proxy_vfs_ns,0,__ATOMIC_RELAXED);
    b[n++]='P';b[n++]='I';b[n++]='D';b[n++]='=';count_u64(b,&n,(uint64_t)RAW(PROXY_SC_getpid,0,0,0,0,0,0));b[n++]=' ';b[n++]='H';b[n++]='=';count_u64(b,&n,handler);b[n++]=' ';b[n++]='V';b[n++]='=';count_u64(b,&n,vfs);b[n++]='\n';
    uint64_t raw_traps=0,raw_calls=0;vfs_raw_count_summary(&raw_traps,&raw_calls);
    b[n++]='R';b[n++]='A';b[n++]='W';b[n++]=' ';count_u64(b,&n,raw_traps);b[n++]=' ';count_u64(b,&n,raw_calls);b[n++]=' ';count_u64(b,&n,raw_traps?raw_calls/raw_traps:0);b[n++]='\n';
    uint64_t raw_categories[6];vfs_raw_category_summary(raw_categories);
    b[n++]='R';b[n++]='A';b[n++]='W';b[n++]='C';b[n++]='A';b[n++]='T';
    for(unsigned i=0;i<6;i++){b[n++]=' ';count_u64(b,&n,raw_categories[i]);}b[n++]='\n';
    uint64_t raw_syscalls[PROXY_SC_count];vfs_raw_syscall_summary(raw_syscalls);
    for(int i=0;i<PROXY_SC_count;i++)if(raw_syscalls[i]){b[n++]='R';b[n++]='A';b[n++]='W';b[n++]='S';b[n++]='C';b[n++]=' ';count_u64(b,&n,(uint64_t)i);b[n++]=' ';count_u64(b,&n,raw_syscalls[i]);b[n++]='\n';}
    for(int i=0;i<PROXY_SC_count;i++){uint64_t calls=__atomic_exchange_n(&proxy_counts[i],0,__ATOMIC_RELAXED);uint64_t ns=__atomic_exchange_n(&proxy_syscall_ns[i],0,__ATOMIC_RELAXED);if(calls){count_u64(b,&n,(uint64_t)i);b[n++]=' ';count_u64(b,&n,calls);b[n++]=' ';count_u64(b,&n,ns);b[n++]='\n';}}
    RAW(PROXY_SC_write,proxy_count_fd,b,n,0,0,0);
    vfs_profile_dump();
}
static void emit(const char *s) { RAW(PROXY_SC_write,proxy_diagnostic_fd,s,p_len(s),0,0,0); }
static void trace_word(const char *label,uint64_t value) {
    emit(label); crash_hex(value); emit(" ");
}
void proxy_log(const char *kind,long nr,long result) {
    char line[128]; size_t n=0;
    const char *prefix="v3 trap "; while(*prefix) line[n++]=*prefix++;
    while(*kind) line[n++]=*kind++;
    line[n++]=' '; long values[2]={nr,result};
    for(int i=0;i<2;i++) {
        unsigned long v=values[i]<0?(unsigned long)(-(values[i]+1))+1:(unsigned long)values[i];
        if(values[i]<0) line[n++]='-';
        char digits[24];int d=0; do {digits[d++]=(char)('0'+v%10);v/=10;} while(v);
        while(d) line[n++]=digits[--d];
        line[n++]=' ';
    }
    const char *label="pid=";while(*label)line[n++]=*label++;
    unsigned long pid=(unsigned long)RAW(PROXY_SC_getpid,0,0,0,0,0,0);char digits[24];unsigned d=0;
    do{digits[d++]=(char)('0'+pid%10);pid/=10;}while(pid);while(d)line[n++]=digits[--d];line[n++]='\n';
    RAW(PROXY_SC_write,proxy_diagnostic_fd,line,n,0,0,0);
}
static int app_allowed(long nr) {
    for(unsigned i=0;i<sizeof(android_allowed)/sizeof(android_allowed[0]);i++)
        if(android_allowed[i]==arch_encode_syscall(nr)) return 1;
    return 0;
}
static unsigned long namespaces;
static long namespace_clone_parent;
static unsigned long namespace_clone_flags;
uintptr_t proxy_clone_resume;
extern char proxy_clone_entry[],proxy_clone_after[];
static unsigned userns_remaining=~0U;
struct sysctl_fd {int fd;struct sysctl_fd *next;};
static struct sysctl_fd *sysctl_fds;
static int sysctl_tracked(int fd){for(struct sysctl_fd *p=sysctl_fds;p;p=p->next)if(p->fd==fd)return 1;return 0;}
static void sysctl_close(int fd){struct sysctl_fd **p=&sysctl_fds;while(*p){struct sysctl_fd *d=*p;if(d->fd==fd){*p=d->next;vfs_free(d);return;}p=&d->next;}}
static void sysctl_dup(int from,int to){if(from==to)return;int tracked=sysctl_tracked(from);sysctl_close(to);if(tracked){struct sysctl_fd *p=vfs_palloc(sizeof(*p));if(p){*p=(struct sysctl_fd){to,sysctl_fds};sysctl_fds=p;}}}
static long sysctl_open(int cloexec){long fd=RAW(PROXY_SC_memfd_create,"v3-userns-limit",cloexec?1:0,0,0,0,0);if(fd<0)return fd;struct sysctl_fd *p=vfs_palloc(sizeof(*p));if(!p){RAW(PROXY_SC_close,fd,0,0,0,0,0);return -ENOMEM;}*p=(struct sysctl_fd){(int)fd,sysctl_fds};sysctl_fds=p;return fd;}
static long sysctl_write(int fd,const char *buf,size_t len){if(!len || len>11)return -EINVAL;unsigned limit=0;size_t k=0;for(;k<len && buf[k]>='0' && buf[k]<='9';k++){unsigned digit=buf[k]-'0';if(limit>(~0U-digit)/10)return -ERANGE;limit=limit*10+digit;}if(!k || (k<len && !(k+1==len && buf[k]=='\n')))return -EINVAL;long r=RAW(PROXY_SC_write,fd,buf,len,0,0,0);if(r>=0)userns_remaining=limit;return r;}

static int self_exe(const char *path){char name[64]="/proc/",digits[24];size_t k=6;unsigned d=0;unsigned long pid=(unsigned long)RAW(PROXY_SC_getpid,0,0,0,0,0,0);do{digits[d++]=(char)('0'+pid%10);pid/=10;}while(pid);while(d)name[k++]=digits[--d];p_copy(name+k,"/exe",5);return p_equal(path,name) || p_equal(path,"/proc/self/exe");}
static int self_cwd(const char *path){char name[64]="/proc/",digits[24];size_t k=6;unsigned d=0;unsigned long pid=(unsigned long)RAW(PROXY_SC_getpid,0,0,0,0,0,0);do{digits[d++]=(char)('0'+pid%10);pid/=10;}while(pid);while(d)name[k++]=digits[--d];p_copy(name+k,"/cwd",5);return p_equal(path,name) || p_equal(path,"/proc/self/cwd") || p_equal(path,"/proc/thread-self/cwd");}
/* filename_create() checks the dentry before the mount's read-only flag.
 * Do the same for virtual packed entries as well as host-backed paths. */
static int path_exists(const char *path) {
    struct proxy_stat st;
    long r=pack_stat(path,&st);
    if(r!=-ENOSYS)return r==0;
    return arch_stat_at(AT_FDCWD,path,&st,AT_SYMLINK_NOFOLLOW)==0;
}
static long path_call(long nr,proxy_arg_t *a,int one,int two) {
    if(!app_allowed(nr))return -ENOSYS;
    int at=(one==1 && nr!=PROXY_SC_inotify_add_watch) || (nr==PROXY_SC_symlinkat && one==2);
    int fd=at?(int)a[one-1]:AT_FDCWD;
    int follow=nr!=PROXY_SC_lgetxattr && nr!=PROXY_SC_llistxattr && nr!=PROXY_SC_lsetxattr && nr!=PROXY_SC_lremovexattr && nr!=PROXY_SC_readlinkat && nr!=PROXY_SC_readlink && nr!=PROXY_SC_unlinkat && nr!=PROXY_SC_renameat && nr!=PROXY_SC_renameat2 && nr!=PROXY_SC_symlinkat && nr!=PROXY_SC_mkdirat && nr!=PROXY_SC_mknodat;
    if(nr==PROXY_SC_newfstatat || nr==PROXY_SC_fchownat || nr==PROXY_SC_utimensat || nr==PROXY_SC_faccessat2 || nr==PROXY_SC_fchmodat2)follow=!(a[nr==PROXY_SC_fchownat?4:3]&AT_SYMLINK_NOFOLLOW);
    if(nr==PROXY_SC_inotify_add_watch)follow=!(a[2]&IN_DONT_FOLLOW);
    if(nr==PROXY_SC_name_to_handle_at)follow=!!(a[4]&AT_SYMLINK_FOLLOW);
    if(nr==PROXY_SC_linkat)follow=(a[4]&AT_SYMLINK_FOLLOW)?VFS_FOLLOW_LINK_SOURCE:0;
    int writing=nr==PROXY_SC_setxattr || nr==PROXY_SC_lsetxattr || nr==PROXY_SC_removexattr || nr==PROXY_SC_lremovexattr || nr==PROXY_SC_setxattrat || nr==PROXY_SC_removexattrat || nr==PROXY_SC_mkdirat || nr==PROXY_SC_unlinkat || nr==PROXY_SC_renameat || nr==PROXY_SC_renameat2 || nr==PROXY_SC_linkat || nr==PROXY_SC_symlinkat || nr==PROXY_SC_fchmodat || nr==PROXY_SC_fchmodat2 || nr==PROXY_SC_fchownat || nr==PROXY_SC_utimensat || nr==PROXY_SC_mknodat || nr==PROXY_SC_truncate || (nr==PROXY_SC_openat && (a[2]&(O_WRONLY|O_RDWR|O_CREAT|O_TRUNC)));
    if(nr==PROXY_SC_getxattrat || nr==PROXY_SC_listxattrat || nr==PROXY_SC_setxattrat || nr==PROXY_SC_removexattrat)follow=!(a[2]&AT_SYMLINK_NOFOLLOW);
    if(nr==PROXY_SC_statx)follow=!(a[2]&AT_SYMLINK_NOFOLLOW);
    if(nr==PROXY_SC_openat)follow=!(a[2]&O_NOFOLLOW) && (a[2]&(O_CREAT|O_EXCL))!=(O_CREAT|O_EXCL);
    /* Empty-path metadata on packed descriptors describes the indexed object,
     * not the harmless kernel carrier used to keep a real fd/dirfd. */
    if(a[one] && !*(char *)(uintptr_t)a[one] && at){
        if(nr==PROXY_SC_readlinkat || nr==PROXY_SC_newfstatat || nr==PROXY_SC_statx){int handled=0;long packed=pack_call(nr,a,&handled);if(handled)return packed;}
        if(nr==PROXY_SC_fchmodat2){struct proxy_stat mode;if(!arch_stat_fd(fd,&mode) && S_ISREG(mode.st_mode))a[2]=(a[2]&~S_ISVTX)|(mode.st_mode&S_ISVTX);}long r=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);if(!r && nr==PROXY_SC_newfstatat)links_payload_fd_stat(fd,(void *)(uintptr_t)a[2]);if(!r && nr==PROXY_SC_statx){struct proxy_stat st;if(!arch_stat_fd(fd,&st)){links_fd_stat(fd,&st);arch_statx_nlink((void *)(uintptr_t)a[4],st.st_nlink);}}return r;}
    const char *original_path=(void *)(uintptr_t)a[one];
    if(proxy_trace && original_path){
        proxy_log("path-input",nr,fd);
        emit("v3 path input ");emit(original_path);emit("\n");
    }
    long stat_flags=nr==PROXY_SC_newfstatat?a[3]:nr==PROXY_SC_statx?a[2]:0;
    if((nr==PROXY_SC_newfstatat || nr==PROXY_SC_statx) && fd==AT_FDCWD &&
            original_path && *original_path && !(stat_flags&AT_NO_AUTOMOUNT) &&
            (nr!=PROXY_SC_statx || (a[3]&STATX_MODE))){
        int fastfd=-1;char *fastpath=NULL;
        if(!vfs_openat2_path_fd(fd,original_path,follow,&fastfd,&fastpath)){
            long fast_result;
            if(nr==PROXY_SC_newfstatat)
                fast_result=RAW(nr,fastfd,"",a[2],a[3]|AT_EMPTY_PATH,0,0);
            else
                fast_result=RAW(nr,fastfd,"",a[2]|AT_EMPTY_PATH,a[3],a[4],0);
            int fallback=0;
            if(!fast_result && nr==PROXY_SC_newfstatat){
                struct stat *st=(void *)(uintptr_t)a[2];
                fallback=S_ISREG(st->st_mode) && (st->st_mode&S_ISVTX);
                if(!fallback)links_payload_path_stat(fastpath,st,0);
            }else if(!fast_result){
                struct statx *sx=(void *)(uintptr_t)a[4];
                fallback=S_ISREG(sx->stx_mode) && (sx->stx_mode&S_ISVTX);
                if(!fallback)links_payload_path_stat(fastpath,sx,1);
            }
            RAW(PROXY_SC_close,fastfd,0,0,0,0,0);
            vfs_free(fastpath);if(!fallback)return fast_result;
        }
    }
    if(nr==PROXY_SC_openat && original_path){long synthetic=vfs_proc_open(original_path,(int)a[2]);if(synthetic!=-ENOSYS)return synthetic;}
    long e=0;void *pm=NULL,*qm=NULL;char *p=writing?vfs_resolve_mount_slow(fd,(char *)(uintptr_t)a[one],follow,&e,&pm):vfs_resolve_mount(fd,(char *)(uintptr_t)a[one],follow,&e,&pm);if(!p){if(proxy_trace && a[one]){const char *in=(const char *)(uintptr_t)a[one];RAW(PROXY_SC_write,proxy_diagnostic_fd,"v3 unresolved ",14,0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,in,p_len(in),0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,"\n",1,0,0,0);}return e;}
    char *q=NULL;
    if(proxy_trace){emit("v3 path resolved ");emit(p);emit("\n");}
    int create_readonly_open=nr==PROXY_SC_openat && (a[2]&O_CREAT) && !(a[2]&(O_WRONLY|O_RDWR|O_TRUNC));
    int exclusive_create=nr==PROXY_SC_openat && (a[2]&(O_CREAT|O_EXCL))==(O_CREAT|O_EXCL);
    if(nr==PROXY_SC_mkdirat && path_exists(p)){
        if(vfs_bwrap_shm_mkdir(original_path)){e=0;goto done;}
        e=-EEXIST;goto done;
    }
    if(exclusive_create && path_exists(p)){e=-EEXIST;goto done;}
    if(create_readonly_open && path_exists(p))a[2]&=~O_CREAT;
    int packed=0;e=pack_path(nr,a,p,&packed);if(packed){if(e>=0 && nr==PROXY_SC_openat)vfs_fd_record(e,fd,original_path);goto done;}
    if(two>=0){q=vfs_resolve_mount(at?(int)a[two-1]:AT_FDCWD,(char *)(uintptr_t)a[two],0,&e,&qm);if(!q)goto done;int packed_dest=0;pack_path(PROXY_SC_unlinkat,a,q,&packed_dest);if(packed_dest){e=-EROFS;goto done;}if(pm!=qm){e=-EXDEV;goto done;}a[two]=(long)q;if(at)a[two-1]=AT_FDCWD;}
    if(nr==PROXY_SC_openat){e=vfs_proc_open(p,(int)a[2]);if(e!=-ENOSYS)goto done;}
    if(writing)vfs_resolve_cache_invalidate();
    if((writing || nr==PROXY_SC_truncate) && (links_marker(p) || (q && links_marker(q)))){e=-EPERM;goto done;}
    if(writing){e=vfs_mount_readonly(pm);int create_pack_root=pack_mount_root(p)&&(nr==PROXY_SC_mkdirat||(nr==PROXY_SC_openat&&(a[2]&O_CREAT)));if(e && create_pack_root)e=0;if(e || (q&&(e=vfs_mount_readonly(qm))))goto done;}
    if((namespaces&CLONE_NEWUSER) && nr==PROXY_SC_openat && (a[2]&(O_WRONLY|O_RDWR)) && p_equal(p,"/proc/sys/user/max_user_namespaces")){e=sysctl_open((a[2]&O_CLOEXEC)!=0);goto done;}
    /* bwrap reads the overflow IDs for its uid maps; Android's policy denies
     * apps the generic proc_sysctl file. Serve the kernel default (65534). */
    if(nr==PROXY_SC_openat && !(a[2]&(O_WRONLY|O_RDWR)) && (p_equal(p,"/proc/sys/kernel/overflowuid") || p_equal(p,"/proc/sys/kernel/overflowgid"))){
        long fd=RAW(PROXY_SC_memfd_create,"v3-overflowid",(a[2]&O_CLOEXEC)?1:0,0,0,0,0);
        if(fd>=0 && (RAW(PROXY_SC_write,fd,"65534\n",6,0,0,0)!=6 || RAW(PROXY_SC_lseek,fd,0,SEEK_SET,0,0,0)!=0)){RAW(PROXY_SC_close,fd,0,0,0,0,0);fd=-EIO;}
        e=fd;goto done;
    }
    if(namespaces && nr==PROXY_SC_openat && (a[2]&(O_WRONLY|O_RDWR))){
        size_t n=p_len(p);const char *tail=p;while(*tail){if(*tail=='/')tail++;else break;}int proc=p[0]=='/' && p[1]=='p' && p[2]=='r' && p[3]=='o' && p[4]=='c' && p[5]=='/';
        if(proc && ((n>=8 && p_equal(p+n-8,"/uid_map")) || (n>=8 && p_equal(p+n-8,"/gid_map")) || (n>=10 && p_equal(p+n-10,"/setgroups")))){e=RAW(PROXY_SC_memfd_create,"v3-namespace-map",(a[2]&O_CLOEXEC)?1:0,0,0,0,0);goto done;}
    }
    /* bwrap unlinks --bind-data/--ro-bind-data temp files right after binding;
     * a real mount keeps the inode, our virtual one needs the name. */
    if(nr==PROXY_SC_unlinkat && !(a[2]&AT_REMOVEDIR) && vfs_is_mount_source(p)){e=0;goto done;}
    /* bwrap --dev needs /dev/full, which Android apps may not open: serve
     * /dev/zero (reads identical; writes succeed instead of ENOSPC). */
    if(p_equal(p,"/dev/full")) {
        struct proxy_stat probe;
        if(arch_stat_at(AT_FDCWD,"/dev/full",&probe,0)==-EACCES) {
            char *zero=vfs_alloc(10);
            if(!zero){e=-ENOMEM;goto done;}
            p_copy(zero,"/dev/zero",10);vfs_free(p);p=zero;
        }
    }
    /* Android mounts devpts with ptmxmode=000: apps use /dev/ptmx, the same
     * multiplexer for this devpts instance. bwrap links /dev/ptmx to
     * pts/ptmx, so serve opens of that node from /dev/ptmx. */
    if(nr==PROXY_SC_openat && p_equal(p,"/dev/pts/ptmx")) {
        struct proxy_stat probe;
        if(!arch_stat_at(AT_FDCWD,"/dev/pts/ptmx",&probe,0) && !(probe.st_mode&0777)) {
            char *ptmx=vfs_alloc(10);
            if(!ptmx){e=-ENOMEM;goto done;}
            p_copy(ptmx,"/dev/ptmx",10);vfs_free(p);p=ptmx;
        }
    }
    if(proxy_trace){RAW(PROXY_SC_write,proxy_diagnostic_fd,"v3 path ",8,0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,p,p_len(p),0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,"\n",1,0,0,0);}
    a[one]=(long)p;if(at)a[one-1]=AT_FDCWD;
    if(nr==PROXY_SC_readlinkat){char *tracked=vfs_proc_fd_name(p);if(tracked){size_t n=p_len(tracked);if(n>(size_t)a[3])n=(size_t)a[3];p_copy((void *)(uintptr_t)a[2],tracked,n);vfs_free(tracked);e=(long)n;goto done;}}
    if(nr==PROXY_SC_readlinkat && self_exe(p)){char *exe=vfs_exe();if(exe){size_t n=p_len(exe);if(n>(size_t)a[3])n=(size_t)a[3];p_copy((void *)(uintptr_t)a[2],exe,n);vfs_free(exe);e=(long)n;goto done;}}
    if(nr==PROXY_SC_readlinkat && self_cwd(p)){long error=0;char *logical=vfs_getcwd(&error);if(!logical){e=error;goto done;}size_t n=p_len(logical);if(n>(size_t)a[3])n=(size_t)a[3];p_copy((void *)(uintptr_t)a[2],logical,n);vfs_free(logical);e=(long)n;goto done;}
    if(nr==PROXY_SC_openat && (a[2]&O_TMPFILE)==O_TMPFILE)e=-EOPNOTSUPP;
    else if(nr==PROXY_SC_linkat)e=links_publish(p,q,a[4]);
    else if(nr==PROXY_SC_unlinkat || nr==PROXY_SC_renameat || nr==PROXY_SC_renameat2)e=links_name_call(nr,a,p,q);
    else if(nr==PROXY_SC_readlinkat || nr==PROXY_SC_symlinkat || nr==PROXY_SC_mkdirat || nr==PROXY_SC_mknodat)e=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
    else e=links_path_call(nr,a,p);
    if(e>=0 && q && (nr==PROXY_SC_renameat || nr==PROXY_SC_renameat2))vfs_fd_rename(p,q);
    if(e>=0 && ((nr==PROXY_SC_openat && (a[2]&O_CREAT)) || nr==PROXY_SC_linkat || nr==PROXY_SC_unlinkat || nr==PROXY_SC_renameat || nr==PROXY_SC_renameat2 || nr==PROXY_SC_symlinkat || nr==PROXY_SC_mkdirat || nr==PROXY_SC_mknodat || nr==PROXY_SC_fchmodat || nr==PROXY_SC_fchmodat2 || nr==PROXY_SC_fchownat))links_cache_invalidate();
    if(e>=0 && nr==PROXY_SC_openat)vfs_fd_record((int)e,fd,original_path);
    if(e>=0 && nr==PROXY_SC_readlinkat){char *raw=vfs_alloc((size_t)e+1);if(!raw){e=-ENOMEM;goto done;}p_copy(raw,(void *)(uintptr_t)a[2],(size_t)e);raw[e]=0;
        /* Only proc magic links expose backing names. Ordinary symlink targets
         * retain their stored text, as required by readlink. */
        if(p_len(p)>=6 && p[0]=='/' && p[1]=='p' && p[2]=='r' && p[3]=='o' && p[4]=='c' && p[5]=='/' && raw[0]=='/'){
            char *rev=vfs_reverse(raw);if(rev){size_t n=p_len(rev);if(n>(size_t)a[3])n=(size_t)a[3];p_copy((void *)(uintptr_t)a[2],rev,n);e=(long)n;vfs_free(rev);}}
        vfs_free(raw);
    }
done:
    /* Hardware passthrough is "where convenient": a host /dev node the app's
     * sandbox may not use is reported absent, so bwrap/Flatpak *-try binds and
     * device probes skip it instead of failing on EACCES. */
    if(p && p[0]=='/' && p[1]=='d' && p[2]=='e' && p[3]=='v' && p[4]=='/') {
        if(e==-EACCES && (nr==PROXY_SC_newfstatat || nr==PROXY_SC_statx || nr==PROXY_SC_faccessat2 || nr==PROXY_SC_faccessat ||
                (nr==PROXY_SC_openat && (a[2]&O_PATH))))e=-ENOENT;
        /* O_PATH opens need no permission, so the denial would only surface at
         * the caller's fstat (bwrap's bind source check): probe it here. */
        if(e>=0 && nr==PROXY_SC_openat && (a[2]&O_PATH)) {
            struct proxy_stat probe;
            if(arch_stat_fd(e,&probe)==-EACCES){RAW(PROXY_SC_close,e,0,0,0,0,0);e=-ENOENT;}
        }
        /* Existence checks (access F_OK) need no permission either; Flatpak
         * uses them to pick a mandatory --dev-bind, so answer consistently. */
        if(e==0 && (nr==PROXY_SC_faccessat || nr==PROXY_SC_faccessat2)) {
            struct proxy_stat probe;
            if(arch_stat_at(AT_FDCWD,p,&probe,0)==-EACCES)e=-ENOENT;
        }
    }
    vfs_free(p);vfs_free(q);return e;
}
/* Shared file-path dispatch for the ordinary SIGSYS route and the first
 * direct-rewrite slice.  Keeping translation here preserves the same VFS,
 * pack and permission behavior on both paths. */
static void dns_close(int);
static void dns_dup(int,int);
static long rewrite_slow(long,proxy_arg_t *);
static long modernize(long,proxy_arg_t *,long *);
static long descriptor_dup_dispatch(long nr,proxy_arg_t *a){
    long result;
    if(vfs_private_fd((int)a[0])||(nr==PROXY_SC_dup3&&private_close_fd((int)a[1])))return -EBADF;
    result=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
        if(result>=0 && (nr!=PROXY_SC_fcntl || a[1]==F_DUPFD || a[1]==F_DUPFD_CLOEXEC)){long tracked=vfs_fd_dup((int)a[0],(int)result);if(tracked<0){RAW(PROXY_SC_close,result,0,0,0,0,0);dns_close((int)result);vfs_fd_close((int)result);net_close((int)result);sysctl_close((int)result);return tracked;}dns_dup((int)a[0],(int)result);net_dup((int)a[0],(int)result);sysctl_dup((int)a[0],(int)result);}
    return result;
}
static long file_dispatch(long nr,proxy_arg_t *a) {
    switch(nr){
    case PROXY_SC_fcntl:return descriptor_dup_dispatch(nr,a);
    case PROXY_SC_openat:case PROXY_SC_newfstatat:case PROXY_SC_statx:case PROXY_SC_readlinkat:
        return path_call(nr,a,1,-1);
    case PROXY_SC_readlink:return path_call(nr,a,0,-1);
    case PROXY_SC_close:{
        if(private_close_fd((int)a[0]))return 0;
        long r=RAW(PROXY_SC_close,a[0],0,0,0,0,0);
        if(!r){dns_close((int)a[0]);vfs_fd_close((int)a[0]);net_close((int)a[0]);sysctl_close((int)a[0]);}
        return r;
    }
    case PROXY_SC_fstat:{
        if(vfs_private_fd((int)a[0]))return -EBADF;
        long r=RAW(nr,a[0],a[1],0,0,0,0);
        if(!r)links_payload_fd_stat((int)a[0],(void *)(uintptr_t)a[1]);
        return r;
    }
    case PROXY_SC_getdents64:
        if(vfs_private_fd((int)a[0]))return -EBADF;
        return links_dents(a[0],(void *)(uintptr_t)a[1],a[2]);
    case PROXY_SC_lseek:
        if(vfs_private_fd((int)a[0]))return -EBADF;
        return RAW(nr,a[0],a[1],a[2],0,0,0);
    default:return -ENOSYS;
    }
}
/* Both entries use the same VFS dispatch; no ucontext is fabricated. */
long proxy_direct_dispatch(long nr,proxy_arg_t *saved) {
    PROFILE_BEGIN();
    /* The entry's save area must remain immutable: modernize/pack dispatch
     * may rewrite arguments, but syscall-preserved registers restore verbatim. */
    proxy_args args;p_copy(args,saved,sizeof(args));proxy_arg_t *a=args;
    nr=arch_decode_syscall(nr);
#if defined(__i386__)
    /* i386 stat64 is the legacy path form; feed the same normalized
     * newfstatat path handling used by the trapped dispatcher. */
    if(nr==PROXY_SC_stat || nr==PROXY_SC_lstat)nr=modernize(nr,a,NULL);
#endif
    if(nr==PROXY_SC_readlink){a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;nr=PROXY_SC_readlinkat;}
    /* Match native filter bypasses, including CLOEXEC on private fds. */
    if(nr==PROXY_SC_fcntl && (a[1]==F_GETFD || a[1]==F_SETFD ||
       a[1]==F_GETFL || a[1]==F_SETFL))return RAW(nr,a[0],a[1],a[2],0,0,0);
    if(nr==PROXY_SC_fcntl && a[1]!=F_GETFD && a[1]!=F_SETFD && a[1]!=F_GETFL &&
       a[1]!=F_SETFL && a[1]!=F_DUPFD && a[1]!=F_DUPFD_CLOEXEC)return rewrite_slow(nr,a);
    struct thread_state *thread;
    {PROFILE_BEGIN();thread=thread_state();PROFILE_END(9);}
    if(thread)thread->direct_depth++;
    uintptr_t ceiling=(uintptr_t)&ceiling;
    void *arena;
    {PROFILE_BEGIN();arena=thread?vfs_direct_begin(thread->tid,ceiling):NULL;PROFILE_END(10);}
    int packed=0;
    long result;
    if(nr!=PROXY_SC_close && nr!=PROXY_SC_openat && nr!=PROXY_SC_newfstatat &&
       nr!=PROXY_SC_statx && nr!=PROXY_SC_readlink && nr!=PROXY_SC_readlinkat && vfs_private_fd((int)a[0]))result=-EBADF;
    else {
        {PROFILE_BEGIN();result=pack_call(nr,a,&packed);if(!packed)result=file_dispatch(nr,a);PROFILE_END(11);}
    }
    vfs_direct_end(arena);
    if(thread)thread->direct_depth--;
    PROFILE_END(8);
    return result;
}
#include "rewrite.h"
static void rewrite_segv_handler(int sig,siginfo_t *info,void *context){
#ifdef ARCH_REWRITE_HEADER
    ucontext_t *uc=context;
    uintptr_t nr=(uintptr_t)info->si_addr,sp=ARCH_CONTEXT(uc,ARCH_STACK),ret=0;
    if(info->si_code==SEGV_MAPERR && nr<512 && (uintptr_t)ARCH_CONTEXT(uc,ARCH_PC)==nr &&
       !proxy_copy(&ret,(void *)sp,sizeof(ret))){
        for(struct rewrite_record *r=__atomic_load_n(&rewrite_records,__ATOMIC_ACQUIRE);r;r=r->next)if(r->active && r->ret==ret && r->nr==nr){
            proxy_arg_t a[6]={ARCH_CONTEXT(uc,ARCH_ARG0),ARCH_CONTEXT(uc,ARCH_ARG1),ARCH_CONTEXT(uc,ARCH_ARG2),ARCH_CONTEXT(uc,ARCH_ARG3),ARCH_CONTEXT(uc,ARCH_ARG4),ARCH_CONTEXT(uc,ARCH_ARG5)};
            ARCH_CONTEXT(uc,ARCH_RESULT)=proxy_direct_dispatch((long)nr,a);
            ARCH_CONTEXT(uc,ARCH_STACK)=sp+sizeof(ret);ARCH_CONTEXT(uc,ARCH_PC)=ret;
            if(rewrite_log)proxy_log("rewrite-race-recovered",ret,nr);
            return;
        }
    }
#endif
    struct kernel_action action=chained_segv;
    if(action.handler<=1){crash_report(sig,info,context);return;}
    if(action.flags&SA_RESETHAND)chained_segv.handler=0;
    proxy_sigmask_t old=0,mask=action.mask&~SIGSYS_BIT;
    RAW(PROXY_SC_rt_sigprocmask,SIG_BLOCK,&mask,&old,8,0,0);
    if(action.flags&SA_SIGINFO)((void (*)(int,siginfo_t *,void *))action.handler)(sig,info,context);
    else ((void (*)(int))action.handler)(sig);
    old&=~SIGSYS_BIT;RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&old,0,8,0,0);
}

/* Translate before path dispatch so forbidden legacy instructions are never
 * replayed under the inherited Android filter. x86_64 stat layouts match. */
/* Flatpak verbose output does not include the sealed --args bundle in this
 * build. Log it without changing the fd offset or its seals. */
void proxy_log_bwrap_args(const char *path,char *const *argv){
    const char *base=path;for(const char *p=path;*p;p++)if(*p=='/')base=p+1;
    if(!p_equal(base,"matonos-app-bwrap") && !p_equal(base,"libmatonos-app-bwrap.so") && !p_equal(base,"bwrap"))return;
    for(size_t i=0;argv && argv[i];i++){
        RAW(PROXY_SC_write,proxy_diagnostic_fd,"v3 bwrap argv ",14,0,0,0);
        RAW(PROXY_SC_write,proxy_diagnostic_fd,argv[i],p_len(argv[i]),0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,"\n",1,0,0,0);
        if(!p_equal(argv[i],"--args") || !argv[i+1])continue;
        const char *n=argv[i+1];unsigned fd=0;int valid=*n!=0;
        for(;*n;n++){if(*n<'0'||*n>'9'||fd>(2147483647U-(unsigned)(*n-'0'))/10){valid=0;break;}fd=fd*10+(*n-'0');}
        if(!valid)continue;
        RAW(PROXY_SC_write,proxy_diagnostic_fd,"v3 bwrap bundle begin\n",sizeof("v3 bwrap bundle begin\n")-1,0,0,0);
        char bytes[4096];long offset=0,count;
        while((count=RAW(PROXY_SC_pread64,fd,bytes,sizeof(bytes),offset,0,0))>0){
            for(long j=0;j<count;j++)if(!bytes[j])bytes[j]='\n';
            RAW(PROXY_SC_write,proxy_diagnostic_fd,bytes,count,0,0,0);offset+=count;
        }
        RAW(PROXY_SC_write,proxy_diagnostic_fd,"v3 bwrap bundle end\n",sizeof("v3 bwrap bundle end\n")-1,0,0,0);
    }
}
static long dns_poll(void *,unsigned long,struct timespec *,const void *,unsigned long);
static long modernize(long nr,proxy_arg_t *a,long *answer) {
    switch(nr){
    case PROXY_SC_access:a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_faccessat;
    case PROXY_SC_open:a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_openat;
    case PROXY_SC_creat:a[3]=a[1];a[2]=O_CREAT|O_WRONLY|O_TRUNC;a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_openat;
    case PROXY_SC_stat:case PROXY_SC_lstat:a[3]=nr==PROXY_SC_lstat?AT_SYMLINK_NOFOLLOW:0;a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_newfstatat;
    case PROXY_SC_mkdir:a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_mkdirat;
    case PROXY_SC_rmdir:case PROXY_SC_unlink:a[2]=nr==PROXY_SC_rmdir?AT_REMOVEDIR:0;a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_unlinkat;
    case PROXY_SC_rename:case PROXY_SC_link:a[3]=a[1];a[1]=a[0];a[0]=a[2]=AT_FDCWD;a[4]=0;return nr==PROXY_SC_link?PROXY_SC_linkat:PROXY_SC_renameat;
    case PROXY_SC_symlink:a[2]=a[1];a[1]=AT_FDCWD;return PROXY_SC_symlinkat;
    case PROXY_SC_readlink:a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_readlinkat;
    case PROXY_SC_chmod:a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_fchmodat;
    case PROXY_SC_chown:case PROXY_SC_lchown:a[4]=nr==PROXY_SC_lchown?AT_SYMLINK_NOFOLLOW:0;a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_fchownat;
    case PROXY_SC_mknod:a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_mknodat;
    case PROXY_SC_io_pgetevents:{
        struct aio_mask {proxy_sigmask_t *mask;size_t size;};struct aio_mask *m=(void *)(uintptr_t)a[5];proxy_sigmask_t old=0;
        if(m && m->mask){if(m->size!=8){*answer=-EINVAL;return -1;}proxy_sigmask_t next=*m->mask&~SIGSYS_BIT;*answer=RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&next,&old,8,0,0);if(*answer)return -1;}
        *answer=RAW(PROXY_SC_io_getevents,a[0],a[1],a[2],a[3],a[4],0);
        if(m && m->mask)RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&old,0,8,0,0);
        return -1;
    }
    case PROXY_SC_accept:a[3]=0;return PROXY_SC_accept4;
    case PROXY_SC_pause:*answer=RAW(PROXY_SC_ppoll,0,0,0,0,8,0);return -1;
    case PROXY_SC_futimesat:{struct timespec times[2],*tp=NULL;if(a[2]){struct timeval *tv=(void *)(uintptr_t)a[2];tp=times;for(int i=0;i<2;i++){times[i].tv_sec=tv[i].tv_sec;times[i].tv_nsec=tv[i].tv_usec*1000;}}proxy_arg_t b[6]={a[0],a[1],(long)tp,0,0,0};*answer=a[1]?path_call(PROXY_SC_utimensat,b,1,-1):RAW(PROXY_SC_utimensat,a[0],0,(long)tp,0,0,0);return -1;}
    case PROXY_SC_poll:{struct timespec ts,*t=NULL;if((int)a[2]>=0){ts.tv_sec=(int)a[2]/1000;ts.tv_nsec=((int)a[2]%1000)*1000000;t=&ts;}*answer=dns_poll((void *)(uintptr_t)a[0],a[1],t,NULL,8);return -1;}
    case PROXY_SC_select:{struct timespec ts,*t=NULL;struct timeval *tv=(void *)(uintptr_t)a[4];if(tv){ts.tv_sec=tv->tv_sec;ts.tv_nsec=tv->tv_usec*1000;t=&ts;}*answer=RAW(PROXY_SC_pselect6,a[0],a[1],a[2],a[3],t,0);if(tv){tv->tv_sec=ts.tv_sec;tv->tv_usec=ts.tv_nsec/1000;}return -1;}
    case PROXY_SC_alarm:{struct itimerval next={0},old;next.it_value.tv_sec=(unsigned)a[0];*answer=RAW(PROXY_SC_setitimer,ITIMER_REAL,&next,&old,0,0,0);if(!*answer)*answer=old.it_value.tv_sec+(old.it_value.tv_usec!=0);return -1;}
    case PROXY_SC_utime:case PROXY_SC_utimes:{struct timespec times[2],*tp=NULL;if(a[1]){tp=times;if(nr==PROXY_SC_utime){long *v=(void *)(uintptr_t)a[1];for(int i=0;i<2;i++){times[i].tv_sec=v[i];times[i].tv_nsec=0;}}else {struct timeval *v=(void *)(uintptr_t)a[1];for(int i=0;i<2;i++){times[i].tv_sec=v[i].tv_sec;times[i].tv_nsec=v[i].tv_usec*1000;}}}proxy_arg_t b[6]={AT_FDCWD,a[0],(long)tp,0,0,0};*answer=path_call(PROXY_SC_utimensat,b,1,-1);return -1;}
    case PROXY_SC_getdents:{
        /* linux_dirent64 -> legacy x86_64 linux_dirent, including d_type
         * in the last byte. Consume at most caller capacity, no dropped entries. */
        void *buf=vfs_alloc((size_t)a[2]);if(!buf){*answer=-ENOMEM;return -1;}long n=links_dents(a[0],buf,a[2]);long used=0;
        if(n>0)for(long off=0;off<n;){unsigned char *p=(unsigned char *)buf+off;unsigned short len=*(unsigned short *)(p+16);if(len<20 || off+len>n){n=-EIO;break;}unsigned char *q=(unsigned char *)a[1]+used;p_copy(q,p,18);size_t name=p_len((char *)p+19)+1;p_copy(q+18,p+19,name);for(size_t k=18+name;k<len;k++)q[k]=0;q[len-1]=p[18];used+=len;off+=len;}
        *answer=n<0?n:used;vfs_free(buf);return -1;
    }
    case PROXY_SC_pipe:a[1]=0;return PROXY_SC_pipe2;
    case PROXY_SC_dup2:
        if(a[0]==a[1]){*answer=RAW(PROXY_SC_fcntl,a[0],F_GETFD,0,0,0,0);if(*answer>=0)*answer=a[0];return -1;}a[2]=0;return PROXY_SC_dup3;
    case PROXY_SC_getpgrp:a[0]=0;return PROXY_SC_getpgid;
    case PROXY_SC_epoll_create:if(a[0]<=0){*answer=-EINVAL;return -1;}a[0]=0;return PROXY_SC_epoll_create1;
    case PROXY_SC_epoll_wait:a[4]=0;a[5]=8;return PROXY_SC_epoll_pwait;
    case PROXY_SC_inotify_init:a[0]=0;return PROXY_SC_inotify_init1;
    case PROXY_SC_eventfd:a[1]=0;return PROXY_SC_eventfd2;
    case PROXY_SC_signalfd:a[3]=0;return PROXY_SC_signalfd4;
    case PROXY_SC_fork:a[0]=SIGCHLD;a[1]=a[2]=a[3]=a[4]=0;return PROXY_SC_clone;
    case PROXY_SC_time:{struct timespec ts;*answer=RAW(PROXY_SC_clock_gettime,0,&ts,0,0,0,0);if(!*answer){*answer=ts.tv_sec;if(a[0])*(long *)(uintptr_t)a[0]=ts.tv_sec;}return -1;}
    default:return nr;
    }
}
/* DNS interception preserves the application's original fd numbers. */
#include "dns-netd.h"
/* Filesystem AF_UNIX addresses are paths: translate them through the virtual
 * mount table like any path call (Wayland, X11, PulseAudio, D-Bus sockets).
 * Abstract and unnamed addresses pass through. Returns 1 when handled. */
static int unix_call(long nr,proxy_arg_t a[6],long *result) {
    if(nr!=PROXY_SC_connect && nr!=PROXY_SC_bind)return 0;
    const struct sockaddr_un *in=(const void *)a[1];
    long len=a[2];
    if(proxy_trace && in && len>2 && len<=(long)sizeof(*in) && in->sun_family==AF_UNIX){
        size_t limit=(size_t)len-offsetof(struct sockaddr_un,sun_path);
        crash_text("v3 unix ");
        if(in->sun_path[0]) {
            size_t n=0;while(n<limit && in->sun_path[n])n++;
            RAW(PROXY_SC_write,proxy_diagnostic_fd,in->sun_path,n,0,0,0);
        } else {
            /* Abstract names are binary, including embedded/trailing NULs. */
            static const char hex[]="0123456789abcdef";
            for(size_t n=0;n<limit;n++) {
                unsigned char c=(unsigned char)in->sun_path[n];
                char escaped[4]={'\\','x',hex[c>>4],hex[c&15]};
                RAW(PROXY_SC_write,proxy_diagnostic_fd,escaped,sizeof(escaped),0,0,0);
            }
        }
        crash_text("\n");
    }
    if(!in || len<=(long)offsetof(struct sockaddr_un,sun_path) || len>(long)sizeof(*in) || in->sun_family!=AF_UNIX || !in->sun_path[0])return 0;
    char given[sizeof(in->sun_path)+1];size_t n=(size_t)len-offsetof(struct sockaddr_un,sun_path);
    p_copy(given,in->sun_path,n);given[n]=0;
    long e=0;char *real=vfs_resolve(AT_FDCWD,given,nr==PROXY_SC_connect,&e);
    if(!real){*result=e;return 1;}
    if(p_equal(real,given)){vfs_free(real);return 0;}
    struct sockaddr_un out;p_copy(&out,in,offsetof(struct sockaddr_un,sun_path));out.sun_family=AF_UNIX;
    size_t k=p_len(real);long dir=-1;
    if(k<sizeof(out.sun_path)) {
        p_copy(out.sun_path,real,k+1);
    } else {
        /* Too long for sun_path: reach it through an O_PATH handle on its dir. */
        char *slash=real+k;while(slash>real && *slash!='/')slash--;
        if(slash==real){vfs_free(real);*result=-ENAMETOOLONG;return 1;}
        *slash=0;dir=RAW(PROXY_SC_openat,AT_FDCWD,real,O_PATH|O_DIRECTORY|O_CLOEXEC,0,0,0);*slash='/';
        if(dir<0){vfs_free(real);*result=dir;return 1;}
        char via[sizeof(out.sun_path)];size_t m=0;const char *pre="/proc/self/fd/";while(*pre)via[m++]=*pre++;
        char digits[24];int d=0;unsigned long v=(unsigned long)dir;do{digits[d++]=(char)('0'+v%10);v/=10;}while(v);while(d)via[m++]=digits[--d];
        size_t tail=p_len(slash);
        if(m+tail>=sizeof(via)){RAW(PROXY_SC_close,dir,0,0,0,0,0);vfs_free(real);*result=-ENAMETOOLONG;return 1;}
        p_copy(via+m,slash,tail+1);p_copy(out.sun_path,via,m+tail+1);k=m+tail;
    }
    vfs_free(real);
    *result=RAW(nr,a[0],(long)&out,(long)(offsetof(struct sockaddr_un,sun_path)+k+1),0,0,0);
    if(dir>=0)RAW(PROXY_SC_close,dir,0,0,0,0,0);
    return 1;
}
/* glibc >= 2.42 sets terminal attributes with the termios2 ioctls, which
 * Android's app policy does not allow on ptys (unpriv_tty_ioctls). termios2 is
 * termios plus two trailing speed fields; serve them with the classic calls.
 * Arbitrary speeds (BOTHER) mean nothing on a pty; they become B38400. */
#include "arch/termios.h"
/* No existing fault-safe pointer helper exists. Let pipe copy_{from,to}_user
 * check both ends; unlike /proc/self/mem this respects read-only mappings.
 * A termios2 fits atomically in an empty pipe; nonblocking avoids any wait. */
long proxy_copy(void *dst,const void *src,size_t size) {
    int fd[2];long r=RAW(PROXY_SC_pipe2,fd,O_CLOEXEC|O_NONBLOCK,0,0,0,0);
    if(r<0)return r;
    r=RAW(PROXY_SC_write,fd[1],src,size,0,0,0);
    if(r==(long)size)r=RAW(PROXY_SC_read,fd[0],dst,size,0,0,0);
    RAW(PROXY_SC_close,fd[0],0,0,0,0,0);RAW(PROXY_SC_close,fd[1],0,0,0,0,0);
    return r==(long)size?0:r<0?r:-EFAULT;
}
static int termios2_call(proxy_arg_t *a,long *result) {
    unsigned long cmd=(unsigned long)a[1]&0xffffffffu;
    struct proxy_termios2 copy={0};
    if(cmd==V3_TCGETS2){
        long r=RAW(PROXY_SC_ioctl,a[0],0x5401,&copy,0,0,0);
        if(!r){unsigned speed=copy.c_cflag&0010017;copy.c_ispeed=copy.c_ospeed=speed==0?0:38400;
            r=proxy_copy((void *)(uintptr_t)a[2],&copy,sizeof(copy));}
        *result=r;return 1;
    }
    if(cmd!=V3_TCSETS2 && cmd!=V3_TCSETSW2 && cmd!=V3_TCSETSF2)return 0;
    *result=proxy_copy(&copy,(void *)(uintptr_t)a[2],sizeof(copy));if(*result)return 1;
    if((copy.c_cflag&0010017)==0010000)copy.c_cflag=(copy.c_cflag&~0010017u)|0000017;
    *result=RAW(PROXY_SC_ioctl,a[0],cmd==V3_TCSETS2?0x5402:cmd==V3_TCSETSW2?0x5403:0x5404,(long)&copy,0,0,0);
    return 1;
}
/* close_range is present in the app filter: intercept it as well as close.
 * Enumerating proc avoids a UINT_MAX loop and preserves private descriptors. */
static long payload_close_range(unsigned first,unsigned last,unsigned flags) {
    if(first>last || (flags&~6u))return -EINVAL;
    if(flags&2u)return -ENOSYS; /* let libc retry without CLOSE_RANGE_UNSHARE */
    long fd=RAW(PROXY_SC_openat,AT_FDCWD,"/proc/self/fd",O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,0,0);
    if(fd<0)return fd;
    char buf[4096];long n;
    while((n=RAW(PROXY_SC_getdents64,fd,buf,sizeof(buf),0,0,0))>0){
        for(long off=0;off<n;){unsigned short len=*(unsigned short *)(buf+off+16);
            if(len<20 || off+len>n){n=-EIO;goto done;}
            const char *p=buf+off+19;off+=len;unsigned value=0;
            if(*p<'0'||*p>'9')continue;
            while(*p>='0'&&*p<='9'){value=value*10+(*p++-'0');}
            if(*p || value<first || value>last || value==(unsigned)fd || private_close_fd((int)value))continue;
            if(flags&4u)RAW(PROXY_SC_fcntl,value,F_SETFD,FD_CLOEXEC,0,0,0);
            else {RAW(PROXY_SC_close,value,0,0,0,0,0);dns_close(value);vfs_fd_close(value);net_close(value);sysctl_close(value);}
        }
    }
done:
    RAW(PROXY_SC_close,fd,0,0,0,0,0);return n<0?n:0;
}
static int ours(long n) {
    if(proxy_rewrite_traps(n))return 1;
    if(ipc_ours(n))return 1;
#if defined(__i386__) && defined(MATON_GPU_BRIDGE)
    if(n==PROXY_SC_set_tid_address || n==PROXY_SC_set_robust_list || n==PROXY_SC_get_robust_list)return 1;
#endif
    switch(n) {
    case PROXY_SC_epoll_ctl:case PROXY_SC_ioctl:case PROXY_SC_socket:case PROXY_SC_bind:case PROXY_SC_getsockname:case PROXY_SC_sendto:case PROXY_SC_sendmsg:case PROXY_SC_recvfrom:case PROXY_SC_recvmsg:case PROXY_SC_connect:case PROXY_SC_getpeername:case PROXY_SC_sendmmsg:case PROXY_SC_setsockopt:case PROXY_SC_getsockopt:
    case PROXY_SC_read:case PROXY_SC_pread64:case PROXY_SC_pwrite64:case PROXY_SC_readv:case PROXY_SC_writev:case PROXY_SC_mmap:case PROXY_SC_ftruncate:case PROXY_SC_lseek:
    case PROXY_SC_preadv:case PROXY_SC_preadv2:case PROXY_SC_pwritev:case PROXY_SC_pwritev2:case PROXY_SC_fallocate:case PROXY_SC_sendfile:case PROXY_SC_copy_file_range:case PROXY_SC_splice:
    case PROXY_SC_getxattr:case PROXY_SC_lgetxattr:case PROXY_SC_listxattr:case PROXY_SC_llistxattr:case PROXY_SC_setxattr:case PROXY_SC_lsetxattr:case PROXY_SC_removexattr:case PROXY_SC_lremovexattr:case PROXY_SC_truncate:
    case PROXY_SC_fchmod:case PROXY_SC_fchown:case PROXY_SC_fsetxattr:case PROXY_SC_fremovexattr:
    case PROXY_SC_close_range:case PROXY_SC_flock:case PROXY_SC_fstat:case PROXY_SC_getdents64:case PROXY_SC_exit:case PROXY_SC_exit_group:case PROXY_SC_write:case PROXY_SC_close:case PROXY_SC_dup:case PROXY_SC_dup3:case PROXY_SC_fcntl:
    case PROXY_SC_capget:case PROXY_SC_capset:case PROXY_SC_setreuid:case PROXY_SC_setregid:case PROXY_SC_setfsuid:case PROXY_SC_setfsgid:case PROXY_SC_setgroups:case PROXY_SC_setuid:case PROXY_SC_setgid:case PROXY_SC_setresuid:case PROXY_SC_setresgid:
#if ARCH_AUDIT == AUDIT_ARCH_I386
    case PROXY_SC_set_thread_area:case PROXY_SC_get_thread_area:case PROXY_SC_modify_ldt:
#endif
    case PROXY_SC_utime:case PROXY_SC_utimes:case PROXY_SC_creat:case PROXY_SC_rmdir:case PROXY_SC_mknod:case PROXY_SC_mknodat:
    case PROXY_SC_prctl:case PROXY_SC_execve:case PROXY_SC_execveat:case PROXY_SC_rt_sigaction:case PROXY_SC_rt_sigprocmask:
    case PROXY_SC_sigaltstack:case PROXY_SC_unshare:case PROXY_SC_mount:case PROXY_SC_umount2:case PROXY_SC_pivot_root:
    case PROXY_SC_vfork:case PROXY_SC_clone:case PROXY_SC_clone3:case PROXY_SC_io_uring_setup:case PROXY_SC_io_uring_enter:case PROXY_SC_io_uring_register:
    case PROXY_SC_open:case PROXY_SC_openat:case PROXY_SC_openat2:case PROXY_SC_open_tree:case PROXY_SC_open_tree_attr:
    case PROXY_SC_mount_setattr:case PROXY_SC_move_mount:case PROXY_SC_fsopen:case PROXY_SC_fsconfig:case PROXY_SC_fsmount:case PROXY_SC_fspick:case PROXY_SC_stat:case PROXY_SC_lstat:case PROXY_SC_newfstatat:
    case PROXY_SC_inotify_add_watch:case PROXY_SC_getxattrat:case PROXY_SC_listxattrat:case PROXY_SC_setxattrat:case PROXY_SC_removexattrat:case PROXY_SC_name_to_handle_at:case PROXY_SC_statx:case PROXY_SC_access:case PROXY_SC_faccessat:case PROXY_SC_faccessat2:case PROXY_SC_readlink:case PROXY_SC_readlinkat:
    case PROXY_SC_fchdir:case PROXY_SC_chdir:case PROXY_SC_getcwd:case PROXY_SC_mkdir:case PROXY_SC_mkdirat:case PROXY_SC_unlink:case PROXY_SC_unlinkat:
    case PROXY_SC_rename:case PROXY_SC_renameat:case PROXY_SC_renameat2:case PROXY_SC_link:case PROXY_SC_linkat:
    case PROXY_SC_symlink:case PROXY_SC_symlinkat:case PROXY_SC_chmod:case PROXY_SC_fchmodat:case PROXY_SC_fchmodat2:case PROXY_SC_chown:case PROXY_SC_lchown:
    case PROXY_SC_fchownat:case PROXY_SC_utimensat:case PROXY_SC_statfs:case PROXY_SC_chroot:return 1;
    default:return 0;
    }
}
static void dispatch_app(int sig,siginfo_t *info,void *ctx) {
    struct kernel_action action=chained;
    if(action.handler==1 && info->si_code<=0) return;
    if(action.handler<=1) { /* SIGSYS default is fatal, including ignored synchronous SIGSYS. */
        /* Name the cause before dying: no tombstone is written for this process. */
        proxy_log("fatal-sigsys",info->si_syscall,info->si_code);
        proxy_log("fatal-sigsys-pc",0,(long)ARCH_CONTEXT((ucontext_t *)ctx,ARCH_PC));
        RAW(PROXY_SC_exit_group,128+sig,0,0,0,0,0);return;
    }
    if(action.flags&SA_RESETHAND) chained.handler=0;
    /* Keep SIGSYS deliverable even during app chaining. Do not implement SA_ONSTACK
     * by switching stacks here; the proxy never depends on an app-owned altstack. */
    proxy_sigmask_t old=0,mask=action.mask&~SIGSYS_BIT;
    RAW(PROXY_SC_rt_sigprocmask,SIG_BLOCK,&mask,&old,8,0,0);
    if(action.flags&SA_SIGINFO) ((void (*)(int,siginfo_t *,void *))action.handler)(sig,info,ctx);
    else ((void (*)(int))action.handler)(sig);
    old&=~SIGSYS_BIT;RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&old,0,8,0,0);
}
static void exec_close_fds(void){
    long fd=RAW(PROXY_SC_openat,AT_FDCWD,"/proc/self/fd",O_DIRECTORY|O_RDONLY|O_CLOEXEC,0,0,0);if(fd<0)return;
    char *buf=vfs_alloc(8192);if(!buf){RAW(PROXY_SC_close,fd,0,0,0,0,0);return;}
    for(;;){long n=RAW(PROXY_SC_getdents64,fd,buf,8192,0,0,0);if(n<=0)break;
        for(long off=0;off<n;){unsigned short len=*(unsigned short *)(buf+off+16);if(len<20 || off+len>n)break;char *name=buf+off+19;long v=0;int valid=*name!=0;for(;*name;name++){if(*name<'0'||*name>'9'){valid=0;break;}v=v*10+*name-'0';}
            /* Proxy-private descriptors back resident state and diagnostics;
             * payload F_SETFD is native now, but must not tear these down. */
            if(valid && v!=fd && !private_close_fd((int)v)){long flags=RAW(PROXY_SC_fcntl,v,F_GETFD,0,0,0,0);if(flags>=0 && (flags&FD_CLOEXEC)){RAW(PROXY_SC_close,v,0,0,0,0,0);dns_close((int)v);vfs_fd_close((int)v);net_close((int)v);sysctl_close((int)v);}}off+=len;
        }
    }
    vfs_free(buf);RAW(PROXY_SC_close,fd,0,0,0,0,0);
}
#include "host-command.c"

/* Syscalls neither emulated nor allowed by Android: report each number once,
 * without --trace, so an app failing on ENOSYS names the missing syscall. */
static void unhandled_once(long id,long kernel_nr){
    static unsigned long seen[(PROXY_SC_count+63)/64];
    if(id>=0 && id<PROXY_SC_count){unsigned long bit=1UL<<(id%64);
        if(__atomic_fetch_or(&seen[id/64],bit,__ATOMIC_RELAXED)&bit)return;}
    proxy_log("unhandled",kernel_nr,id);
}
/* Android loads platform libraries lazily, after the initial syscall filter
 * snapshot. Their syscall sites keep native Android semantics. */
struct android_pc_cache_entry {volatile uintptr_t begin,end;volatile unsigned long epoch;int state;};
static struct android_pc_cache_entry android_pc_cache[256];
static volatile unsigned long android_pc_cache_epoch=1;
static int android_library_mapping(uintptr_t pc,uintptr_t *begin,uintptr_t *end) {
    long fd=RAW(PROXY_SC_openat,AT_FDCWD,"/proc/self/maps",O_RDONLY|O_CLOEXEC,0,0,0);
    if(fd<0)return 0;
    char input[4096],line[4096];size_t used=0;int found=0;
    for(;;){
        long n=RAW(PROXY_SC_read,fd,input,sizeof(input),0,0,0);
        if(n<=0)break;
        for(long i=0;i<n;i++){
            char c=input[i];
            if(c!='\n'){if(used+1<sizeof(line))line[used++]=c;continue;}
            line[used]=0;used=0;
            const char *p=line;uintptr_t lo=resident_maps_hex(&p),hi=0;
            if(*p=='-'){p++;hi=resident_maps_hex(&p);}
            if(pc<lo||pc>=hi)continue;
            while(*p==' ')p++;
            /* p begins at the permissions column, followed by offset, device,
             * and inode before the path in /proc/self/maps. */
            for(unsigned field=0;field<4&&*p;field++){
                while(*p&&*p!=' ')p++;
                while(*p==' ')p++;
            }
            *begin=lo;*end=hi;
            if(resident_android_library_path(p)) found=1;
#if defined(__i386__) && defined(MATON_GPU_BRIDGE)
            /* i386 bionic and glibc share the kernel's syscall trampoline.
             * Its origin is per-call TLS state, never a permanent exemption. */
            if(p_equal(p,"[vdso]"))found=2;
#endif
            goto done;
        }
    }
done:
    RAW(PROXY_SC_close,fd,0,0,0,0,0);
    return found;
}
static int android_library_mapping_state(uintptr_t pc) {
    unsigned long epoch=__atomic_load_n(&android_pc_cache_epoch,__ATOMIC_ACQUIRE);
    for(size_t i=0;i<sizeof(android_pc_cache)/sizeof(android_pc_cache[0]);i++) {
        struct android_pc_cache_entry *entry=&android_pc_cache[i];
        int state=__atomic_load_n(&entry->state,__ATOMIC_ACQUIRE);
        unsigned long cached_epoch=__atomic_load_n(&entry->epoch,__ATOMIC_RELAXED);
        uintptr_t begin=__atomic_load_n(&entry->begin,__ATOMIC_RELAXED),end=__atomic_load_n(&entry->end,__ATOMIC_RELAXED);
        if(state>0&&cached_epoch==epoch&&pc>=begin&&pc<end)return state;
        if(state==-2&&cached_epoch==epoch&&pc>=begin&&pc<end)return 0;
        if(state && state!=-1 && cached_epoch!=epoch){int stale=state;__atomic_compare_exchange_n(&entry->state,&stale,0,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);}
    }
    uintptr_t begin=0,end=0;
    int state=android_library_mapping(pc,&begin,&end);
    if(!state && !(begin<end && pc>=begin && pc<end))return 0;
    for(size_t i=0;i<sizeof(android_pc_cache)/sizeof(android_pc_cache[0]);i++) {
        int expected=0;
        if(__atomic_compare_exchange_n(&android_pc_cache[i].state,&expected,-1,0,
                __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            __atomic_store_n(&android_pc_cache[i].begin,begin,__ATOMIC_RELAXED);
            __atomic_store_n(&android_pc_cache[i].end,end,__ATOMIC_RELAXED);
            __atomic_store_n(&android_pc_cache[i].epoch,__atomic_load_n(&android_pc_cache_epoch,__ATOMIC_ACQUIRE),__ATOMIC_RELAXED);
            __atomic_store_n(&android_pc_cache[i].state,state?state:-2,__ATOMIC_RELEASE);
            break;
        }
    }
    return state;
}
static void android_pc_cache_invalidate(void){
    __atomic_add_fetch(&android_pc_cache_epoch,1,__ATOMIC_RELEASE);
    for(size_t i=0;i<sizeof(android_pc_cache)/sizeof(android_pc_cache[0]);i++)
        __atomic_store_n(&android_pc_cache[i].state,0,__ATOMIC_RELEASE);
}
#include "payload-filter.h"
static void handler(int sig,siginfo_t *info,void *opaque) {
    void *paused_direct=vfs_direct_pause();
    uint64_t count_started=proxy_count_fd>=0?proxy_count_now():0;
    ucontext_t *uc=opaque;
    volatile unsigned char *selector=dispatch_selector;
    unsigned char saved=0;
    if(selector) {saved=*selector;*selector=SYSCALL_DISPATCH_FILTER_ALLOW;}
    /* The retried clone inherits this pending record. Only its child adopts
     * new namespace flags; the parent's monitor keeps the original namespace. */
    if(namespace_clone_parent){
        if(RAW(PROXY_SC_getpid,0,0,0,0,0,0)!=namespace_clone_parent){
            vfs_raw_count_fork_child();
            ipc_fork();
            namespaces|=namespace_clone_flags;
            if((namespace_clone_flags&CLONE_NEWUSER) && userns_remaining!=~0U)userns_remaining--;
        }
        namespace_clone_parent=0;namespace_clone_flags=0;
    }
    struct thread_state *thread=NULL;
    if(proxy_trace) {
        stack_t ss={0};RAW(PROXY_SC_sigaltstack,0,&ss,0,0,0,0);
        emit("v3 handler-stack ");trace_word("local=",(uintptr_t)&ss);
        trace_word("interrupted=",ARCH_CONTEXT(uc,ARCH_STACK));
        trace_word("base=",(uintptr_t)ss.ss_sp);trace_word("size=",ss.ss_size);
        trace_word("flags=",ss.ss_flags);emit("\n");
    }
    uintptr_t internal_pc=(uintptr_t)info->si_call_addr;
    struct thread_state *direct=NULL;
    if(info->si_code==SYS_SECCOMP &&
       (internal_pc==arch_gateway_pc(0) || internal_pc+2==arch_gateway_pc(0)))direct=thread_state();
    int internal_trap=(direct && direct->direct_depth) || proxy_rewrite_internal_site(internal_pc);
    if(info->si_code!=SYS_SECCOMP || internal_trap) {
        if(proxy_trace) {
            emit("v3 signal-delivery sig="); crash_hex(sig);
            trace_word(" code=",info->si_code);
            trace_word(" syscall=",(uint64_t)info->si_syscall);
            trace_word(" call_addr=",(uintptr_t)info->si_call_addr);
            trace_word(" pc=",ARCH_CONTEXT(uc,ARCH_PC)); emit("\n");
        }
        dispatch_app(sig,info,opaque);
        (*arch_signal_mask(uc))&=~SIGSYS_BIT;
        RAW(PROXY_SC_sigaltstack,0,arch_signal_stack(uc),0,0,0,0);
        arch_signal_stack_ready(uc);
        if(selector && dispatch_selector==selector) *selector=saved;
        vfs_direct_resume(paused_direct);
        return;
    }
    vfs_trap_resume();
    PROFILE_BEGIN();
    thread=thread_state();
    if(thread)vfs_trap_begin(thread->signal_stack,thread->signal_stack_size);
    uint64_t raw_calls_at_entry=proxy_raw_count_now();
    long nr,original;
    proxy_arg_t a[6]={ARCH_CONTEXT(uc,ARCH_ARG0),ARCH_CONTEXT(uc,ARCH_ARG1),
        ARCH_CONTEXT(uc,ARCH_ARG2),ARCH_CONTEXT(uc,ARCH_ARG3),
        ARCH_CONTEXT(uc,ARCH_ARG4),ARCH_CONTEXT(uc,ARCH_ARG5)};
    struct arch_call call=arch_decode_call(info->si_syscall,a);
    nr=original=call.id;
    long count_id=nr;
    if(proxy_count_fd>=0 && nr>=0 && nr<PROXY_SC_count)__atomic_fetch_add(&proxy_counts[nr],1,__ATOMIC_RELAXED);
    long result=-ENOSYS;
    if(call.error){result=call.error;goto finish;}
    proxy_rewrite_trap_site((uintptr_t)info->si_call_addr,info->si_syscall);
    uintptr_t syscall_pc=(uintptr_t)info->si_call_addr;
    int syscall_mapping=android_library_mapping_state(syscall_pc);
#if defined(__i386__) && defined(MATON_GPU_BRIDGE)
    extern int adopt_i386_native(void);
    if(syscall_mapping==2)syscall_mapping=state_gpu_bridge && adopt_i386_native();
#endif
    if(proxy_trace && (nr==PROXY_SC_mmap || nr==PROXY_SC_ioctl) && syscall_mapping) {
        emit("v3 graphics-route nr=");crash_hex((uint64_t)nr);
        trace_word(" native=",syscall_mapping);
        trace_word(" pc=",syscall_pc);
        trace_word(" a0=",a[0]);trace_word(" a1=",a[1]);
        trace_word(" a2=",a[2]);trace_word(" a3=",a[3]);
        trace_word(" a4=",a[4]);trace_word(" a5=",a[5]);emit("\n");
    }
    if(syscall_mapping) {
        result=resident_fd_native(nr,a);
        if(proxy_trace && (nr==PROXY_SC_mmap || nr==PROXY_SC_ioctl)) {
            emit("v3 graphics-result nr=");crash_hex((uint64_t)nr);
            trace_word(" result=",result);emit("\n");
        }
        ARCH_CONTEXT(uc,ARCH_RESULT)=arch_finish_call(call,result);
        goto clean_mask;
    }
#if defined(__i386__) && defined(MATON_GPU_BRIDGE)
    extern int adopt_i386_syscall(long,proxy_arg_t *,void *,long *);
    if(adopt_i386_syscall(nr,a,uc,&result))goto finish;
#endif
    /* signalfd readiness is evaluated against the thread's signal state.
     * Infinite poll must wait outside the SIGSYS frame, with the application's
     * mask restored, rather than nesting the wait in a signal handler. */
    if(nr==PROXY_SC_poll && (int)a[2]<0){
        ARCH_CONTEXT(uc,ARCH_RESULT)=arch_encode_syscall(PROXY_SC_ppoll);
        ARCH_CONTEXT(uc,ARCH_ARG2)=0;ARCH_CONTEXT(uc,ARCH_ARG3)=0;
        ARCH_CONTEXT(uc,ARCH_ARG4)=8;arch_restart_syscall(uc);
        if(proxy_trace)proxy_log("android-poll-retry",nr,0);
        goto clean_mask;
    }
    /* Metadata/ownership descriptors are capabilities, never payload I/O. */
    switch(nr){
    case PROXY_SC_mmap:if(a[4]>=0 && vfs_private_fd(a[4])){result=-EBADF;goto finish;}break;
    case PROXY_SC_read:case PROXY_SC_write:case PROXY_SC_pread64:case PROXY_SC_pwrite64:
    case PROXY_SC_readv:case PROXY_SC_writev:case PROXY_SC_ftruncate:case PROXY_SC_lseek:
    case PROXY_SC_preadv:case PROXY_SC_preadv2:case PROXY_SC_pwritev:case PROXY_SC_pwritev2:case PROXY_SC_fallocate:case PROXY_SC_fchmod:case PROXY_SC_fchown:case PROXY_SC_fsetxattr:case PROXY_SC_fremovexattr:
    case PROXY_SC_fstat:case PROXY_SC_getdents64:
        if(vfs_private_fd(a[0])){result=-EBADF;goto finish;}break;
    default:break;
    }
    if((nr==PROXY_SC_sendfile && (vfs_private_fd(a[0])||vfs_private_fd(a[1]))) ||
       ((nr==PROXY_SC_copy_file_range||nr==PROXY_SC_splice) && (vfs_private_fd(a[0])||vfs_private_fd(a[2])))){result=-EBADF;goto finish;}
    nr=modernize(nr,a,&result);
    if(nr<0)goto finish;
    if(nr==PROXY_SC_mmap && !(a[3]&MAP_ANONYMOUS) && (a[2]&PROT_EXEC)){result=proxy_file_mmap(a);goto finish;}
    int packed=0;result=pack_call(nr,a,&packed);if(packed)goto finish;
    if(ipc_ours(nr)){result=ipc_call(nr,a);goto finish;}
    int net_handled=0;result=net_call(nr,a,(namespaces&CLONE_NEWNET)!=0,&net_handled);
    if(net_handled)goto finish;
    result=-ENOSYS;
    if(!ours(nr)) {if(app_allowed(nr))result=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);goto finish;}
    if(nr==PROXY_SC_fchmod){struct proxy_stat mode;if(!arch_stat_fd((int)a[0],&mode) && S_ISREG(mode.st_mode))a[1]=(a[1]&~S_ISVTX)|(mode.st_mode&S_ISVTX);}
    switch(nr) {
    case PROXY_SC_mprotect:case PROXY_SC_munmap:case PROXY_SC_mremap:
        result=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);break;
    case PROXY_SC_read:case PROXY_SC_pread64:case PROXY_SC_pwrite64:case PROXY_SC_readv:case PROXY_SC_writev:case PROXY_SC_mmap:case PROXY_SC_ftruncate:
        result=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);break;
    case PROXY_SC_preadv:case PROXY_SC_preadv2:case PROXY_SC_pwritev:case PROXY_SC_pwritev2:case PROXY_SC_fallocate:case PROXY_SC_sendfile:case PROXY_SC_copy_file_range:case PROXY_SC_splice:case PROXY_SC_fchmod:case PROXY_SC_fchown:case PROXY_SC_fsetxattr:case PROXY_SC_fremovexattr:
        if(app_allowed(nr))result=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
        break;
    case PROXY_SC_getxattr:case PROXY_SC_lgetxattr:case PROXY_SC_listxattr:case PROXY_SC_llistxattr:case PROXY_SC_setxattr:case PROXY_SC_lsetxattr:case PROXY_SC_removexattr:case PROXY_SC_lremovexattr:case PROXY_SC_truncate:result=path_call(nr,a,0,-1);break;
    case PROXY_SC_ppoll:if(app_allowed(nr))result=dns_poll((void *)(uintptr_t)a[0],a[1],(void *)(uintptr_t)a[2],(void *)(uintptr_t)a[3],a[4]);break;
    case PROXY_SC_flock:result=vfs_private_fd((int)a[0])?-EBADF:(app_allowed(nr)?RAW(nr,a[0],a[1],0,0,0,0):-ENOSYS);break;
    case PROXY_SC_lseek:case PROXY_SC_readlink:case PROXY_SC_readlinkat:
        result=file_dispatch(nr,a);break;
    case PROXY_SC_getdents64:result=file_dispatch(nr,a);break;
    case PROXY_SC_fstat:result=file_dispatch(nr,a);break;
    case PROXY_SC_exit:proxy_rewrite_exit();vfs_direct_thread_exit(RAW(PROXY_SC_gettid,0,0,0,0,0,0));ipc_thread_exit();thread_state_cache_forget(thread);proxy_count_dump();result=RAW(nr,a[0],0,0,0,0,0);break;
    case PROXY_SC_exit_group:proxy_rewrite_exit();if(proxy_trace)proxy_log("exit-status",nr,a[0]);ipc_exit();proxy_count_dump();result=RAW(nr,a[0],0,0,0,0,0);break;
    case PROXY_SC_write:result=sysctl_tracked((int)a[0])?sysctl_write((int)a[0],(void *)(uintptr_t)a[1],a[2]):RAW(nr,a[0],a[1],a[2],0,0,0);break;
    case PROXY_SC_close_range:result=payload_close_range((unsigned)a[0],(unsigned)a[1],(unsigned)a[2]);break;
    case PROXY_SC_close:result=file_dispatch(nr,a);break;
    case PROXY_SC_dup:case PROXY_SC_dup3:case PROXY_SC_fcntl:
        result=descriptor_dup_dispatch(nr,a);break;
    case PROXY_SC_epoll_ctl:case PROXY_SC_ioctl:case PROXY_SC_socket:case PROXY_SC_bind:case PROXY_SC_getsockname:case PROXY_SC_sendto:case PROXY_SC_sendmsg:case PROXY_SC_recvfrom:case PROXY_SC_recvmsg:case PROXY_SC_connect:case PROXY_SC_getpeername:case PROXY_SC_sendmmsg:case PROXY_SC_setsockopt:case PROXY_SC_getsockopt:
        if(nr==PROXY_SC_ioctl && vfs_private_fd((int)a[0])){result=-EBADF;break;}
        if(nr==PROXY_SC_ioctl && termios2_call(a,&result)){if(proxy_trace)proxy_log("tty-ioctl",a[1],result);break;}
        /* Reflink clone: Android app data (ext4/f2fs) has none, and app domains may not issue it,
         * so each attempt is an AVC denial. Answer as a non-reflink filesystem would. */
        if(nr==PROXY_SC_ioctl && ((unsigned long)a[1]==FICLONE || (unsigned long)a[1]==FICLONERANGE)){result=-EOPNOTSUPP;break;}
        if(app_allowed(nr)) { if(!unix_call(nr,a,&result))result=dns_call(nr,a); }
        if(nr==PROXY_SC_ioctl && proxy_trace)proxy_log("tty-ioctl",a[1],result);
        break;
    case PROXY_SC_prctl:
        if(proxy_trace) {
            emit("v3 prctl option="); crash_hex(a[0]);
            trace_word(" arg2=",a[1]); trace_word(" arg3=",a[2]);
            trace_word(" arg4=",a[3]); trace_word(" arg5=",a[4]);
        }
        if(a[0]==PR_CAPBSET_DROP || a[0]==PR_CAP_AMBIENT || a[0]==PR_SET_KEEPCAPS) {result=0;break;}
        if(a[0]==PR_CAPBSET_READ){result=0;break;}
        if(a[0]==PR_SET_SYSCALL_USER_DISPATCH) {
            long tid=RAW(PROXY_SC_gettid,0,0,0,0,0,0);
            if(a[1]==PR_SYS_DISPATCH_ON) {
                uintptr_t restore=(uintptr_t)proxy_restore;
                /* The kernel may dispatch sigreturn itself. Reject an interval
                 * that would trap our restorer; never silently widen app policy. */
                if(!a[4] || (dispatch_owner && dispatch_owner!=tid) ||
                   restore<(uintptr_t)a[2] || restore+16>=(uintptr_t)a[2]+(uintptr_t)a[3]) {
                    result=-ENOTSUP;break;
                }
                volatile unsigned char *next=(volatile unsigned char *)a[4];
                unsigned char previous=*next;*next=SYSCALL_DISPATCH_FILTER_ALLOW;
                result=RAW(nr,a[0],a[1],a[2],a[3],a[4],0);
                if(!result) {dispatch_selector=next;dispatch_owner=tid;
                    if(!selector) {selector=next;saved=previous;}
                } else *next=previous;
            } else if(a[1]==PR_SYS_DISPATCH_OFF) {
                result=RAW(nr,a[0],a[1],a[2],a[3],a[4],0);
                if(!result && dispatch_owner==tid) {dispatch_selector=NULL;dispatch_owner=0;}
            } else result=-EINVAL;
        } else result=payload_filter(nr,a);
        if(proxy_trace) {emit("result=");crash_hex(result);emit("\n");}
        break;
    case PROXY_SC_seccomp:
        if(proxy_trace) {
            emit("v3 seccomp op="); crash_hex(a[0]);
            trace_word(" flags=",a[1]); trace_word(" args=",a[2]);
        }
        result=payload_filter(nr,a);
        if(proxy_trace) {emit("result=");crash_hex(result);emit("\n");}
        break;
    case PROXY_SC_rt_sigaction:
        if(a[0]==SIGSYS) {
            if(a[3]!=8) {result=-EINVAL;break;}
            struct kernel_action next=chained;
            if(a[1]) p_copy(&next,(void *)(uintptr_t)a[1],sizeof(next));
            if(a[2]) p_copy((void *)(uintptr_t)a[2],&chained,sizeof(chained));
            if(a[1]) {next.mask&=~SIGSYS_BIT;chained=next;}result=0;
        }
        else if(a[0]==SIGSEGV) {
            if(a[3]!=8){result=-EINVAL;break;}
            struct kernel_action next=chained_segv;
            if(a[1])p_copy(&next,(void *)(uintptr_t)a[1],sizeof(next));
            if(a[2])p_copy((void *)(uintptr_t)a[2],&chained_segv,sizeof(chained_segv));
            if(a[1]){next.mask&=~SIGSYS_BIT;chained_segv=next;}
#if ARCH_AUDIT == AUDIT_ARCH_I386
            struct kernel_action proxy={(uintptr_t)ldt_segv_handler,SA_SIGINFO|SA_RESTORER|SA_ONSTACK,(uintptr_t)proxy_restore,0};
#else
            struct kernel_action proxy={(uintptr_t)rewrite_segv_handler,SA_SIGINFO|SA_RESTORER|SA_ONSTACK,(uintptr_t)proxy_restore,0};
#endif
            result=RAW(nr,SIGSEGV,&proxy,0,8,0,0);
        }
        else {
            struct kernel_action action;
            if(a[1]) {p_copy(&action,(void *)(uintptr_t)a[1],sizeof(action));action.mask&=~SIGSYS_BIT;a[1]=(long)&action;}
            result=RAW(nr,a[0],a[1],a[2],a[3],0,0);
        }break;
    case PROXY_SC_rt_sigprocmask: {
        if(a[3]!=8) {result=-EINVAL;break;}
        proxy_sigmask_t mask;
        if(a[1]) {p_copy(&mask,(void *)(uintptr_t)a[1],8);mask&=~SIGSYS_BIT;a[1]=(long)&mask;}
        result=RAW(nr,a[0],a[1],a[2],8,0,0);
        if(!result && a[1])RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,0,&(*arch_signal_mask(uc)),8,0,0);
        break;
    }
#if ARCH_AUDIT == AUDIT_ARCH_I386
    case PROXY_SC_set_thread_area:
        result=thread?tls_set(thread,(uintptr_t)a[0]):-ENOMEM;break;
    case PROXY_SC_get_thread_area:
        result=thread?tls_get(thread,(uintptr_t)a[0]):-ENOMEM;break;
    case PROXY_SC_modify_ldt:
        result=ldt_call((unsigned)a[0],(uintptr_t)a[1],(size_t)a[2]);break;
#endif
    case PROXY_SC_sigaltstack: {
        /* App altstack changes are virtual: the kernel retains our private stack.
         * The chained app handler's SA_ONSTACK is currently unsupported. */
        if(!thread) {result=-ENOMEM;break;}
        stack_t next=thread->app_stack;
        if(a[0]) {
            p_copy(&next,(void *)(uintptr_t)a[0],sizeof(next));
            if(next.ss_flags & ~(SS_DISABLE|(1U<<31))) {result=-EINVAL;break;}
            if(!(next.ss_flags&SS_DISABLE) && next.ss_size<2048) {result=-ENOMEM;break;}
        }
        if(a[1]) p_copy((void *)(uintptr_t)a[1],&thread->app_stack,sizeof(stack_t));
        if(a[0]) thread->app_stack=next;
        result=0;break;
    }
    case PROXY_SC_unshare:
        result=(a[0]&~(long)NS_FLAGS)?-ENOSYS:0;
        if(!result && (a[0]&CLONE_NEWUSER) && !userns_remaining)result=-EPERM;
        if(!result){namespaces|=a[0];if((a[0]&CLONE_NEWUSER) && userns_remaining!=~0U)userns_remaining--;}
        break;
    case PROXY_SC_mount:result=vfs_mount((char *)(uintptr_t)a[0],(char *)(uintptr_t)a[1],(char *)(uintptr_t)a[2],(unsigned long)a[3]);break;
    case PROXY_SC_umount2:result=vfs_unmount((char *)(uintptr_t)a[0]);break;
    case PROXY_SC_pivot_root:result=vfs_pivot((char *)(uintptr_t)a[0],(char *)(uintptr_t)a[1]);break;
    case PROXY_SC_chroot:result=vfs_root((char *)(uintptr_t)a[0]);break;
    case PROXY_SC_setreuid:case PROXY_SC_setregid:case PROXY_SC_setfsuid:case PROXY_SC_setfsgid:case PROXY_SC_setgroups:case PROXY_SC_setuid:case PROXY_SC_setgid:case PROXY_SC_setresuid:case PROXY_SC_setresgid:result=0;break;
    case PROXY_SC_capset:result=0;break;
    case PROXY_SC_capget:{struct __user_cap_header_struct *h=(void *)(uintptr_t)a[0];if(!h){result=-EFAULT;break;}
        unsigned count=h->version==_LINUX_CAPABILITY_VERSION_1?1:2;
        if(h->version!=_LINUX_CAPABILITY_VERSION_1 && h->version!=_LINUX_CAPABILITY_VERSION_2 && h->version!=_LINUX_CAPABILITY_VERSION_3){h->version=_LINUX_CAPABILITY_VERSION_3;result=-EINVAL;break;}
        if(a[1]){struct __user_cap_data_struct *d=(void *)(uintptr_t)a[1];for(unsigned i=0;i<count;i++)d[i]=(struct __user_cap_data_struct){0};}result=0;break;
    }
    case PROXY_SC_vfork:
        /* Fork-like clone is safe inside the handler only with no child stack;
         * preserve libc vfork's nonstandard return-address registers. */
        result=ipc_fork_prepare();if(result)break;
        dns_fork_prepare();result=RAW(PROXY_SC_clone,SIGCHLD,0,0,0,0,0);dns_fork_result(result);ipc_fork_result(result);if(!result){thread_state_cache_invalidate_all();vfs_raw_count_fork_child();ipc_fork();}break;
    case PROXY_SC_clone:
        /* Stackless forks can clone inside this handler. A supplied child
         * stack needs the assembly retry gateway to preserve its continuation. */
        if((a[0]&NS_FLAGS) && (a[0]&CLONE_VM)){result=-ENOTSUP;break;}
        if((a[0]&CLONE_NEWUSER) && !userns_remaining){result=-EPERM;break;}
        if(!(a[0]&CLONE_VM)){
            if(a[0]&CLONE_FILES){result=-ENOTSUP;break;}
            if(a[1])goto clone_retry;
            unsigned long ns=a[0]&NS_FLAGS;result=ipc_fork_prepare();if(result)break;
            dns_fork_prepare();
            result=RAW(PROXY_SC_clone,a[0]&~(long)(NS_FLAGS|CLONE_VFORK),0,a[2],a[3],a[4],0);
            dns_fork_result(result);ipc_fork_result(result);

            if(!result){thread_state_cache_invalidate_all();vfs_raw_count_fork_child();ipc_fork();namespaces|=ns;if((ns&CLONE_NEWUSER) && userns_remaining!=~0U)userns_remaining--;}
            break;
        }
clone_retry:
        if(!(a[0]&CLONE_VM) || (a[0]&CLONE_VFORK)){result=ipc_fork_prepare();if(result)break;dns_fork_prepare();(*arch_signal_mask(uc))=~SIGSYS_BIT;}
        namespace_clone_parent=RAW(PROXY_SC_getpid,0,0,0,0,0,0);namespace_clone_flags=a[0]&NS_FLAGS;
        ARCH_CONTEXT(uc,ARCH_ARG1)=a[1];ARCH_CONTEXT(uc,ARCH_ARG2)=a[2];
        ARCH_CONTEXT(uc,ARCH_ARG3)=a[3];ARCH_CONTEXT(uc,ARCH_ARG4)=a[4];
        ARCH_CONTEXT(uc,ARCH_ARG0)=a[0]&~(long)(NS_FLAGS|CLONE_VFORK|((a[0]&CLONE_VFORK)?CLONE_VM:0));
        proxy_clone_resume=ARCH_CONTEXT(uc,ARCH_PC);
        ARCH_CONTEXT(uc,ARCH_RESULT)=arch_encode_syscall(nr);ARCH_CONTEXT(uc,ARCH_PC)=(uintptr_t)proxy_clone_entry;
        if(proxy_trace)proxy_log("ours-clone-retry",nr,0);
        goto clean_mask;
    case PROXY_SC_clone3:result=-ENOSYS;break; /* libc falls back to clone. */
    case PROXY_SC_execve:case PROXY_SC_execveat: {
        const char *path=(const char *)(uintptr_t)a[nr==PROXY_SC_execve?0:1];
        if(nr==PROXY_SC_execveat && (a[0]!=AT_FDCWD || a[4])) {result=-ENOSYS;break;}
        if(!thread) {result=-ENOMEM;break;}
        proxy_count_dump();
        if(proxy_trace){emit("v3 exec ");emit(path);emit("\n");}
        struct image image;
        result=proxy_load(path,(char *const *)(uintptr_t)a[nr==PROXY_SC_execve?1:2],(char *const *)(uintptr_t)a[nr==PROXY_SC_execve?2:3],&image);
        if(!result) {
#if ARCH_AUDIT == AUDIT_ARCH_I386
            if(ldt_entries){vfs_free(ldt_entries);ldt_entries=NULL;ldt_capacity=0;}
#endif
            vfs_network_payload((namespaces&CLONE_NEWNET)!=0);
            proxy_host_snapshot((char *const *)(uintptr_t)a[nr==PROXY_SC_execve?2:3]);
            ipc_exec();
            exec_close_fds();
            exec_signals();
            /* exec starts the new linker without the previous libc TLS. */
            ARCH_CLEAR_TLS();
            proxy_log("ours-exec",nr,0);
            /* A real exec resets caught signal actions. Our resident action stays. */
            if(chained.handler!=1) chained.handler=0;
#if ARCH_AUDIT == AUDIT_ARCH_I386
            if(chained_segv.handler!=1)chained_segv.handler=0;
#endif
            if(dispatch_selector) {
                RAW(PROXY_SC_prctl,PR_SET_SYSCALL_USER_DISPATCH,PR_SYS_DISPATCH_OFF,0,0,0,0);
                dispatch_selector=NULL;dispatch_owner=0;
            }
            if(thread) thread->app_stack.ss_flags=SS_DISABLE;
            ARCH_CONTEXT(uc,ARCH_PC)=image.entry;ARCH_CONTEXT(uc,ARCH_STACK)=image.stack;
            arch_exec_context(uc);
        }break;
    }
    case PROXY_SC_chdir:result=vfs_chdir(AT_FDCWD,(char *)(uintptr_t)a[0]);break;
    case PROXY_SC_fchdir:result=vfs_chdir((int)a[0],NULL);break;
    case PROXY_SC_open:case PROXY_SC_stat:case PROXY_SC_lstat:case PROXY_SC_access:
    case PROXY_SC_mkdir:case PROXY_SC_unlink:case PROXY_SC_chmod:case PROXY_SC_chown:case PROXY_SC_lchown:case PROXY_SC_statfs:
        result=path_call(nr,a,0,-1);break;
    case PROXY_SC_openat2:result=-ENOSYS;break; /* bwrap uses its checked openat fallback. */
    /* Raw open_tree would resolve the untranslated path. ENOSYS makes libglnx
     * chase (Flatpak) fall back to openat, which is translated. */
    case PROXY_SC_open_tree:case PROXY_SC_open_tree_attr:result=-ENOSYS;break;
    /* Mounts are emulated through classic mount(); bwrap falls back to
     * MS_REMOUNT when the new mount API reports ENOSYS. */
    case PROXY_SC_mount_setattr:case PROXY_SC_move_mount:case PROXY_SC_fsopen:case PROXY_SC_fsconfig:case PROXY_SC_fsmount:case PROXY_SC_fspick:
        result=-ENOSYS;break;
    /* futimens(fd): a NULL path names the fd itself; there is no path to map. */
    case PROXY_SC_utimensat:if(!a[1]){result=app_allowed(nr)?RAW(PROXY_SC_utimensat,a[0],0,a[2],a[3],0,0):-ENOSYS;break;}result=path_call(nr,a,1,-1);break;
    case PROXY_SC_openat:case PROXY_SC_newfstatat:case PROXY_SC_statx:
        result=file_dispatch(nr,a);break;
    case PROXY_SC_inotify_add_watch:case PROXY_SC_getxattrat:case PROXY_SC_listxattrat:case PROXY_SC_setxattrat:case PROXY_SC_removexattrat:case PROXY_SC_name_to_handle_at:case PROXY_SC_faccessat:case PROXY_SC_faccessat2:
    case PROXY_SC_mknodat:case PROXY_SC_mkdirat:case PROXY_SC_unlinkat:case PROXY_SC_fchmodat:case PROXY_SC_fchmodat2:case PROXY_SC_fchownat:
        result=path_call(nr,a,1,-1);break;
    case PROXY_SC_link:case PROXY_SC_rename:result=path_call(nr,a,0,1);break;
    case PROXY_SC_linkat:case PROXY_SC_renameat:case PROXY_SC_renameat2:result=path_call(nr,a,1,3);break;
    case PROXY_SC_symlink:result=path_call(nr,a,1,-1);break;
    case PROXY_SC_symlinkat:result=path_call(nr,a,2,-1);break;
    case PROXY_SC_getcwd:{long e=0;char *rev=vfs_getcwd(&e);result=e;
        if(rev){size_t k=p_len(rev)+1;if(k>(size_t)a[1])result=-ERANGE;else {p_copy((void *)(uintptr_t)a[0],rev,k);result=(long)k;}vfs_free(rev);}break;
    }
    default:break;
    }
finish:
    proxy_rewrite_after(nr,a,result);
    if(result>=0 && (nr==PROXY_SC_mmap || nr==PROXY_SC_munmap || nr==PROXY_SC_mremap))android_pc_cache_invalidate();
    ARCH_CONTEXT(uc,ARCH_RESULT)=arch_finish_call(call,result);
    if(proxy_trace){
        proxy_log(!app_allowed(original)?"android":(ours(original)?"ours":"android"),original,result);
        /* Path translation may perform its own metadata probes. Keep the
         * triggering userspace syscall site in the trace so SELinux reports
         * can be tied back to the caller's executable mapping. */
        if(nr==PROXY_SC_open || nr==PROXY_SC_openat || nr==PROXY_SC_openat2 ||
           nr==PROXY_SC_stat || nr==PROXY_SC_lstat || nr==PROXY_SC_newfstatat ||
           nr==PROXY_SC_statx || nr==PROXY_SC_access || nr==PROXY_SC_faccessat ||
           nr==PROXY_SC_faccessat2 || nr==PROXY_SC_readlink || nr==PROXY_SC_readlinkat){
            emit("v3 path-call syscall=");crash_hex((uint64_t)original);
            emit(" ip=");crash_hex((uintptr_t)info->si_call_addr);emit("\n");
        }
    }
    else if(result==-ENOSYS)unhandled_once(original,info->si_syscall);
clean_mask:
    if(proxy_trace){
        uint64_t raw_calls=proxy_raw_count_now()-raw_calls_at_entry;
        emit("v3 raw-calls syscall=");crash_hex((uint64_t)info->si_syscall);
        emit(" count=");crash_hex(raw_calls);
        emit(" openat=");crash_hex(proxy_raw_count_for(PROXY_SC_openat));
        emit(" readlinkat=");crash_hex(proxy_raw_count_for(PROXY_SC_readlinkat));
        emit(" newfstatat=");crash_hex(proxy_raw_count_for(PROXY_SC_newfstatat));
        emit(" geteuid=");crash_hex(proxy_raw_count_for(PROXY_SC_geteuid));
        emit(" mmap=");crash_hex(proxy_raw_count_for(PROXY_SC_mmap));
        emit(" munmap=");crash_hex(proxy_raw_count_for(PROXY_SC_munmap));emit("\n");
    }
    if(count_started){uint64_t ended=proxy_count_now();if(ended>=count_started){uint64_t elapsed=ended-count_started;__atomic_fetch_add(&proxy_handler_ns,elapsed,__ATOMIC_RELAXED);if(count_id>=0 && count_id<PROXY_SC_count)__atomic_fetch_add(&proxy_syscall_ns[count_id],elapsed,__ATOMIC_RELAXED);}}
    PROFILE_END(8);
    /* sigreturn must never restore a blocked SIGSYS, including pthread masks. */
    (*arch_signal_mask(uc))&=~SIGSYS_BIT;
    RAW(PROXY_SC_sigaltstack,0,arch_signal_stack(uc),0,0,0,0);
    vfs_trap_end();
    vfs_direct_resume(paused_direct);
    arch_signal_stack_ready(uc);
    if(selector && dispatch_selector==selector) *selector=saved;
}
/* Resident entry deliberately shares syscall translation, not installation.
 * Its caller owns thread routing and must intercept process-global operations. */
#include ARCH_FILTER_HEADER
static struct arch_ip_range *resident_bionic_ranges(size_t *count, int *error) {
    *count=0; *error=0;
    long fd=RAW(PROXY_SC_openat,AT_FDCWD,"/proc/self/maps",O_RDONLY|O_CLOEXEC,0,0,0);
    if(fd<0) { *error=(int)fd; return NULL; }
    size_t capacity=4096, used=0, range_capacity=0;
    char *maps=RESIDENT_ALLOC(capacity+1);
    struct arch_ip_range *ranges=NULL;
    if(!maps) { RAW(PROXY_SC_close,fd,0,0,0,0,0); *error=-ENOMEM; return NULL; }
    for(;;) {
        if(used==capacity) {
            if(capacity>(SIZE_MAX-1)/2) { *error=-EOVERFLOW; break; }
            size_t next=capacity*2;
            char *grown=RESIDENT_ALLOC(next+1);
            if(!grown) { *error=-ENOMEM; break; }
            p_copy(grown,maps,used);
            RESIDENT_FREE(maps);
            maps=grown; capacity=next;
        }
        long n=RAW(PROXY_SC_read,fd,maps+used,capacity-used,0,0,0);
        if(n==0) break;
        if(n<0) { *error=(int)n; break; }
        used+=(size_t)n;
    }
    RAW(PROXY_SC_close,fd,0,0,0,0,0);
    if(*error) { RESIDENT_FREE(maps); RESIDENT_FREE(ranges); return NULL; }
    maps[used]=0;
    *error=resident_ranges_parse(maps,count,&ranges,&range_capacity);
    if(*error) { *count=0; RESIDENT_FREE(maps); RESIDENT_FREE(ranges); return NULL; }
    RESIDENT_FREE(maps);
    return ranges;
}
int proxy_install(int android) {
    ipc_init(); /* IPC failure must never prevent payload startup. */
    int diagnostics=proxy_diagnostics_init();if(diagnostics)return diagnostics;
    /* Android has no /dev/shm. Give this app proxy instance a private
     * cache-backed POSIX shm directory before any payload or nested bwrap
     * tries to create its own namespaces. Failure must not block launch. */
    (void)vfs_shm_init();
    if(!thread_state()) return -ENOMEM;
    exec_signals();
    struct kernel_action action={(uintptr_t)handler,SA_SIGINFO|SA_RESTORER|SA_NODEFER|SA_ONSTACK,(uintptr_t)proxy_restore,0};
    long r=RAW(PROXY_SC_rt_sigaction,SIGSYS,&action,0,8,0,0);if(r) return (int)r;
    proxy_sigmask_t mask=SIGSYS_BIT;r=RAW(PROXY_SC_rt_sigprocmask,SIG_UNBLOCK,&mask,0,8,0,0);if(r) return (int)r;
    r=RAW(PROXY_SC_prctl,PR_SET_NO_NEW_PRIVS,1,0,0,0,0);if(r) return (int)r;
    if(android) {emit("v3 installing android filter\n");r=arch_app_install();if(r) return (int)r;emit("v3 android filter installed\n");}
    size_t libc_count=0; int range_error=0;
    struct arch_ip_range *libc=resident_bionic_ranges(&libc_count,&range_error);
    if(range_error) return range_error;
    if(proxy_trace) {
        proxy_log("resident-bionic-ranges",(long)libc_count,libc_count?(long)libc[0].begin:0);
        for(size_t i=0;i<libc_count;i++) {
            proxy_log("resident-bionic-range",(long)libc[i].begin,(long)libc[i].end);
            if(libc[i].linker) proxy_log("resident-linker-range",(long)libc[i].begin,(long)libc[i].end);
        }
    }
    int result=arch_filter_install(libc,libc_count);
    RESIDENT_FREE(libc);
    return result;
}


void proxy_state(struct state *s){
 extern const char *proxy_tmpdir;
 uint64_t count;
 chained.handler=state_word(s,chained.handler==1?1:0);
 proxy_trace=state_word(s,proxy_trace);proxy_diagnostic_fd=state_word(s,proxy_diagnostic_fd);
 namespaces=state_word(s,namespaces);userns_remaining=state_word(s,userns_remaining);
 state_string(s,(char **)&proxy_tmpdir);
 state_string(s,(char **)&host_command_socket);host_command_started=state_word(s,host_command_started);
 count=0;for(struct sysctl_fd *d=sysctl_fds;d;d=d->next)if(state_live(d->fd))count++;
 count=state_count(s,count);if(s->error)return;
 if(!s->reading){for(struct sysctl_fd *d=sysctl_fds;d;d=d->next)if(state_live(d->fd))state_word(s,d->fd);}
 else for(uint64_t i=0;i<count&&!s->error;i++){struct sysctl_fd *d=vfs_palloc(sizeof(*d));if(!d){s->error=-ENOMEM;return;}d->fd=state_word(s,0);d->next=sysctl_fds;sysctl_fds=d;}
}
