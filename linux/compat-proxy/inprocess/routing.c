/* SPDX-License-Identifier: Apache-2.0
 * Stage-one resident runner: seccomp belongs only to Linux/adopted threads.
 * ART threads keep their inherited filter and SIGSEGV/SIGBUS actions. */
#define _GNU_SOURCE
#include "routing.h"
#include "../proxy.h"
#include "../../gpu-forward-spike/spike3/adopt.h"
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/auxv.h>
#include <pthread.h>
#include <stddef.h>
#include <errno.h>
#include <elf.h>
#include <dlfcn.h>
int inprocess_status;
struct resident_session *inprocess_session;
int inprocess_output[2]={1,2};
/* Kernel ABI: libc/sigchain trampolines may read FS before our handler can
 * restore it. Install only SIGSYS directly; leave ART fault actions intact. */
static struct resident_signal_action {
    void (*handler)(int,siginfo_t *,void *);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long mask;
} previous;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int signals_ready;
static uintptr_t libc_lo,libc_hi;
int inprocess_signals(void (*handler)(int,siginfo_t *,void *)) {
    if(signals_ready)return 0;
    Dl_info lib;
    if(!dladdr((void *)pthread_create,&lib))return -ENOTSUP;
    const Elf64_Ehdr *eh=lib.dli_fbase;
    const Elf64_Phdr *ph=(void *)((uintptr_t)eh+eh->e_phoff);
    for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD && (ph[i].p_flags&PF_X)) {
        libc_lo=(uintptr_t)eh+ph[i].p_vaddr;libc_hi=libc_lo+ph[i].p_memsz;
    }
    if(!libc_lo || (libc_lo>>32)!=(libc_hi>>32))return -ENOTSUP;
    extern void proxy_restore(void);
    struct resident_signal_action action={handler,SA_SIGINFO|SA_ONSTACK|SA_NODEFER|0x04000000UL,
                                 proxy_restore,0};
    long error=proxy_raw(SYS_rt_sigaction,SIGSYS,(long)&action,(long)&previous,8,0,0);
    if(error)return (int)error;
    signals_ready=1;return 0;
}
void inprocess_chain(int sig,siginfo_t *info,void *context) {
    if(previous.flags&SA_SIGINFO) {
        if(previous.handler)previous.handler(sig,info,context);
    } else if((void *)previous.handler!=(void *)SIG_DFL &&
              (void *)previous.handler!=(void *)SIG_IGN) {
        union { void (*extended)(int,siginfo_t *,void *); void (*simple)(int); } action;
        action.extended=previous.handler;
        action.simple(sig);
    } else __builtin_trap();
}
long inprocess_filter(void) {
    extern char proxy_clone_entry[];
    uintptr_t lo=(uintptr_t)proxy_raw,hi=(uintptr_t)proxy_clone_entry;
    if((lo>>32)!=(hi>>32))return -EINVAL;
    struct sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_rt_sigreturn,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)+4),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,lo>>32,0,4),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)),
        BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,(uint32_t)lo,0,2),
        BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,(uint32_t)hi,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)+4),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,libc_lo>>32,0,4),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)),
        BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,(uint32_t)libc_lo,0,2),
        BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,(uint32_t)libc_hi,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP)
    };
    struct sock_fprog prog={sizeof(code)/sizeof(code[0]),code};
    long error=proxy_raw(SYS_prctl,PR_SET_NO_NEW_PRIVS,1,0,0,0,0);
    return error?error:proxy_raw(SYS_seccomp,SECCOMP_SET_MODE_FILTER,0,(long)&prog,0,0,0);
}
struct launch { char *const *argv;char *const *env;int result; };
static void *run(void *opaque) {
    struct launch *l=opaque;
    extern void inprocess_aux(unsigned,uintptr_t);
    inprocess_aux(AT_HWCAP,getauxval(AT_HWCAP));
    inprocess_aux(AT_HWCAP2,getauxval(AT_HWCAP2));
    inprocess_aux(AT_SYSINFO_EHDR,getauxval(AT_SYSINFO_EHDR));
    struct image image;
    l->result=proxy_load(l->argv[0],l->argv,l->env,&image);
    if(l->result)return NULL;
    uintptr_t maps[6];extern void inprocess_image_maps(uintptr_t[6]);
    inprocess_image_maps(maps);
    if(resident_session_image(inprocess_session,maps)){extern void inprocess_unload(void);inprocess_unload();l->result=-ENOMEM;return NULL;}
    /* Auxv comes from ART's process, never from the JNI caller's stack. */
    uintptr_t *p=(uintptr_t *)image.stack; p+=*p+2;while(*p)p++;
    for(Elf64_auxv_t *a=(void *)(p+1);a->a_type;a++)
        if(a->a_type==AT_HWCAP || a->a_type==AT_HWCAP2 || a->a_type==AT_SYSINFO_EHDR)
            a->a_un.a_val=getauxval(a->a_type);
    if(adopt_install()){l->result=-ENOTSUP;return NULL;}
    inprocess_status=0;
    inprocess_enter(image.entry,image.stack);
    extern void inprocess_wait_threads(void);inprocess_wait_threads();
    l->result=inprocess_status;
    extern void inprocess_forget_image(void);inprocess_forget_image();
    return NULL;
}
int inprocess_run_fds(char *const argv[],char *const env[],const int fds[2]) {
    if(!argv || !argv[0] || !env)return -EINVAL;
    /* One image per stub; concurrent launches must not share proxy state. */
    if(pthread_mutex_trylock(&lock))return -EBUSY;
    inprocess_output[0]=fds[0];inprocess_output[1]=fds[1];
    inprocess_session=resident_session_begin(fds);
    if(!inprocess_session){pthread_mutex_unlock(&lock);return -ENOMEM;}
    struct launch l={argv,env,0};pthread_t thread;
    int e=pthread_create(&thread,NULL,run,&l);
    if(!e)e=pthread_join(thread,NULL);
    resident_session_end(inprocess_session);inprocess_session=NULL;
    pthread_mutex_unlock(&lock);
    return e?-e:l.result;
}

int inprocess_run(char *const argv[],char *const env[]) {
    const int fds[]={1,2};return inprocess_run_fds(argv,env,fds);
}
