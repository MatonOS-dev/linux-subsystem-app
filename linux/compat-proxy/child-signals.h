/* SPDX-License-Identifier: Apache-2.0
 * Resident exec leaves old pthreads alive. A process-directed SIGCHLD that
 * reaches one of them must go to the current payload waiter, not disappear
 * with SIG_DFL. Keep real signalfds and real wait statuses: only a notification
 * delivered to the wrong thread is forwarded. No child is waited for here.
 */
#ifndef MATON_CHILD_SIGNALS_H
#define MATON_CHILD_SIGNALS_H
#include <sys/signalfd.h>
#include <poll.h>
#define CHLD_BIT (UINT64_C(1)<<(SIGCHLD-1))
#define CHLD_FORWARD_TAG 0x4d43484c
struct child_waiter {
    struct child_waiter *next;
    long pid,tid;
    unsigned long generation;
    int fd; /* -1: sigtimedwait */
    unsigned active;
};
struct child_forward {
    struct child_forward *next;
    long pid,tid;
    unsigned pending;
    siginfo_t info;
};
static struct child_waiter *child_waiters;
static struct child_forward *child_forwards;
static unsigned child_pending;
static struct kernel_action child_action;
static long child_image_pid,child_image_tid;
static unsigned long child_generation;
/* Identify payload generations at clone publication, before a new pthread
 * can run. A pre-exec sibling must not acquire the new image's ownership by
 * making its first syscall late or by opening another signalfd after exec. */
struct child_identity {
    struct child_identity *next;
    long pid,tid;
    unsigned long generation;
};
static struct child_identity *child_identities;
static struct child_identity *child_identity(long pid,long tid) {
    for(struct child_identity *p=__atomic_load_n(&child_identities,__ATOMIC_ACQUIRE);p;p=p->next)
        if(p->pid==pid && p->tid==tid)return p;
    struct child_identity *p=vfs_palloc(sizeof(*p));if(!p)return NULL;
    p->pid=pid;p->tid=tid;p->generation=child_generation;
    do{p->next=__atomic_load_n(&child_identities,__ATOMIC_ACQUIRE);}
    while(!__atomic_compare_exchange_n(&child_identities,&p->next,p,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED));
    return p;
}
static unsigned long child_thread_generation(void) {
    struct child_identity *p=child_identity(RAW(PROXY_SC_getpid,0,0,0,0,0,0),RAW(PROXY_SC_gettid,0,0,0,0,0,0));
    return p?__atomic_load_n(&p->generation,__ATOMIC_ACQUIRE):0;
}
void proxy_payload_thread_created(long tid) {
    long pid=RAW(PROXY_SC_getpid,0,0,0,0,0,0);
    unsigned long generation=child_thread_generation();
    struct child_identity *p=child_identity(pid,tid);
    if(p)__atomic_store_n(&p->generation,generation,__ATOMIC_RELEASE);
    for(struct child_waiter *w=__atomic_load_n(&child_waiters,__ATOMIC_ACQUIRE);w;w=w->next)
        if(w->pid==pid && __atomic_load_n(&w->tid,__ATOMIC_ACQUIRE)==tid)
            __atomic_store_n(&w->active,0,__ATOMIC_RELEASE);
    for(struct child_forward *f=__atomic_load_n(&child_forwards,__ATOMIC_ACQUIRE);f;f=f->next)
        if(f->pid==pid && f->tid==tid && __atomic_exchange_n(&f->pending,0,__ATOMIC_ACQ_REL)==2)
            __atomic_sub_fetch(&child_pending,1,__ATOMIC_RELEASE);
}
static void child_signal(int,siginfo_t *,void *);
static void child_install(void) {
    struct kernel_action action=child_action;
    if(action.handler!=1) {
        action.handler=(uintptr_t)child_signal;
        action.flags=(action.flags&(SA_NOCLDSTOP|SA_NOCLDWAIT|SA_RESTART))|
                     SA_SIGINFO|SA_RESTORER|SA_ONSTACK;
        action.restorer=(uintptr_t)proxy_restore;
        action.mask=CHLD_BIT;
    }
    RAW(PROXY_SC_rt_sigaction,SIGCHLD,&action,0,8,0,0);
}
static void child_exec(void) {
    if(!child_image_pid){
        struct kernel_action inherited={0};
        if(!RAW(PROXY_SC_rt_sigaction,SIGCHLD,0,&inherited,8,0,0) && inherited.handler==1)
            child_action=inherited;
    }
    long pid=RAW(PROXY_SC_getpid,0,0,0,0,0,0);
    if(child_image_pid!=pid)__atomic_store_n(&child_pending,0,__ATOMIC_RELEASE);
    child_image_pid=pid;
    child_image_tid=RAW(PROXY_SC_gettid,0,0,0,0,0,0);
    __atomic_add_fetch(&child_generation,1,__ATOMIC_ACQ_REL);
    struct child_identity *self=child_identity(child_image_pid,child_image_tid);
    if(self)__atomic_store_n(&self->generation,child_generation,__ATOMIC_RELEASE);
    if(child_action.handler!=1)child_action=(struct kernel_action){0};
    child_install();
}
static long child_sigaction(proxy_arg_t *a) {
    if(a[3]!=8)return -EINVAL;
    struct kernel_action next=child_action;
    if(a[1] && proxy_copy(&next,(void *)(uintptr_t)a[1],sizeof(next)))return -EFAULT;
    if(a[2] && proxy_copy((void *)(uintptr_t)a[2],&child_action,sizeof(child_action)))return -EFAULT;
    if(a[1]){normalize_payload_action(&next);child_action=next;child_install();}
    return 0;
}
/* Records remain allocated, like the thread registry: a signal handler may
 * hold a published pointer while another thread closes or updates its fd. */
static struct child_waiter *child_wait_begin(int fd) {
    long pid=RAW(PROXY_SC_getpid,0,0,0,0,0,0),tid=RAW(PROXY_SC_gettid,0,0,0,0,0,0);
    struct child_waiter *w;
    for(w=__atomic_load_n(&child_waiters,__ATOMIC_ACQUIRE);w;w=w->next)
        if(w->pid==pid && w->fd==fd && (fd>=0 || w->tid==tid))break;
    if(!w){
        w=vfs_palloc(sizeof(*w));if(!w)return NULL;
        w->pid=pid;w->fd=fd;
        do{w->next=__atomic_load_n(&child_waiters,__ATOMIC_ACQUIRE);}
        while(!__atomic_compare_exchange_n(&child_waiters,&w->next,w,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED));
    }
    __atomic_store_n(&w->active,0,__ATOMIC_RELEASE);
    __atomic_store_n(&w->tid,tid,__ATOMIC_RELAXED);
    __atomic_store_n(&w->generation,child_thread_generation(),__ATOMIC_RELAXED);
    __atomic_store_n(&w->active,1,__ATOMIC_RELEASE);
    return w;
}
static void child_fd_close(int fd) {
    if(!__atomic_load_n(&child_waiters,__ATOMIC_ACQUIRE))return;
    long pid=RAW(PROXY_SC_getpid,0,0,0,0,0,0);
    for(struct child_waiter *w=__atomic_load_n(&child_waiters,__ATOMIC_ACQUIRE);w;w=w->next)
        if(w->pid==pid && w->fd==fd)__atomic_store_n(&w->active,0,__ATOMIC_RELEASE);
}
static void child_fd_dup(int from,int to) {
    if(from==to || !__atomic_load_n(&child_waiters,__ATOMIC_ACQUIRE))return;
    long pid=RAW(PROXY_SC_getpid,0,0,0,0,0,0);int source=0;
    for(struct child_waiter *w=__atomic_load_n(&child_waiters,__ATOMIC_ACQUIRE);w;w=w->next)
        if(w->pid==pid && w->fd==from && __atomic_load_n(&w->active,__ATOMIC_ACQUIRE)){source=1;break;}
    child_fd_close(to);
    if(source)child_wait_begin(to);
}
static void child_fd_claim(int fd) {
    if(!__atomic_load_n(&child_waiters,__ATOMIC_ACQUIRE))return;
    long pid=RAW(PROXY_SC_getpid,0,0,0,0,0,0);
    for(struct child_waiter *w=__atomic_load_n(&child_waiters,__ATOMIC_ACQUIRE);w;w=w->next)
        if(w->pid==pid && w->fd==fd && __atomic_load_n(&w->active,__ATOMIC_ACQUIRE)) {
            long tid=RAW(PROXY_SC_gettid,0,0,0,0,0,0);
            struct child_identity *self=child_identity(pid,tid);
            __atomic_store_n(&w->generation,self?__atomic_load_n(&self->generation,__ATOMIC_ACQUIRE):0,__ATOMIC_RELAXED);
            __atomic_store_n(&w->tid,tid,__ATOMIC_RELEASE);
        }
}
static void child_poll_claim(const struct pollfd *fds,unsigned long count) {
    int active=0;
    for(struct child_waiter *w=__atomic_load_n(&child_waiters,__ATOMIC_ACQUIRE);w;w=w->next)
        if(w->fd>=0 && __atomic_load_n(&w->active,__ATOMIC_ACQUIRE)){active=1;break;}
    if(!active)return;
    for(unsigned long i=0;i<count;i++){
        struct pollfd p;if(proxy_copy(&p,fds+i,sizeof(p)))break;
        if(p.events&POLLIN)child_fd_claim(p.fd);
    }
}
static struct child_forward *child_forward_slot(long pid,long tid) {
    for(struct child_forward *p=__atomic_load_n(&child_forwards,__ATOMIC_ACQUIRE);p;p=p->next)
        if(p->pid==pid && p->tid==tid)return p;
    struct child_forward *p=vfs_palloc(sizeof(*p));if(!p)return NULL;
    p->pid=pid;p->tid=tid;
    do{p->next=__atomic_load_n(&child_forwards,__ATOMIC_ACQUIRE);}
    while(!__atomic_compare_exchange_n(&child_forwards,&p->next,p,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED));
    return p;
}
static int child_unwrap(siginfo_t *info) {
    if(info->si_signo!=SIGCHLD || info->si_code!=SI_QUEUE || info->si_errno!=CHLD_FORWARD_TAG)return 0;
    for(struct child_forward *p=__atomic_load_n(&child_forwards,__ATOMIC_ACQUIRE);p;p=p->next)
        if(p==(void *)info->si_value.sival_ptr && p->pid==RAW(PROXY_SC_getpid,0,0,0,0,0,0) &&
           __atomic_load_n(&p->pending,__ATOMIC_ACQUIRE)==2) {
            p_copy(info,&p->info,sizeof(*info));
            __atomic_sub_fetch(&child_pending,1,__ATOMIC_RELEASE);
            __atomic_store_n(&p->pending,0,__ATOMIC_RELEASE);return 1;
        }
    return 0;
}
static void child_signal(int sig,siginfo_t *info,void *context) {
    long pid=RAW(PROXY_SC_getpid,0,0,0,0,0,0),tid=RAW(PROXY_SC_gettid,0,0,0,0,0,0);
    child_unwrap(info);
    long target=child_image_pid==pid?child_image_tid:pid;
    /* An active synchronous waiter takes precedence over an idle signalfd.
     * Otherwise choose an open signalfd belonging to the current image. */
    for(int synchronous=1;synchronous>=0;synchronous--){
        int found=0;
        for(struct child_waiter *w=__atomic_load_n(&child_waiters,__ATOMIC_ACQUIRE);w;w=w->next)
            if(w->pid==pid && (w->fd==-1)==synchronous &&
               __atomic_load_n(&w->generation,__ATOMIC_ACQUIRE)==child_generation &&
               __atomic_load_n(&w->active,__ATOMIC_ACQUIRE)) {
                long owner=__atomic_load_n(&w->tid,__ATOMIC_ACQUIRE);
                if(RAW(PROXY_SC_tgkill,pid,owner,0,0,0,0)<0)continue;
                target=owner;found=1;break;
            }
        if(found)break;
    }
    /* Explicitly thread-directed payload signals retain their chosen target.
     * Forwarded signals are unwrapped before checking the current owner again. */
    if(info->si_code!=SI_TKILL && tid!=target) {
        struct child_forward *p=child_forward_slot(pid,target);
        unsigned zero=0;
        if(p && __atomic_compare_exchange_n(&p->pending,&zero,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
            p_copy(&p->info,info,sizeof(*info));
            __atomic_add_fetch(&child_pending,1,__ATOMIC_RELEASE);
            __atomic_store_n(&p->pending,2,__ATOMIC_RELEASE);
            siginfo_t queued={0};queued.si_signo=sig;queued.si_code=SI_QUEUE;
            queued.si_errno=CHLD_FORWARD_TAG;queued.si_pid=info->si_pid;queued.si_uid=info->si_uid;
            queued.si_value.sival_ptr=p;
            long r=RAW(PROXY_SC_rt_tgsigqueueinfo,pid,target,sig,&queued,0,0);
            if(r<0){__atomic_sub_fetch(&child_pending,1,__ATOMIC_RELEASE);__atomic_store_n(&p->pending,0,__ATOMIC_RELEASE);}
            if(proxy_trace)proxy_log("chld-route",target,r);
            if(r>=0)return;
        } else if(p)return; /* Standard SIGCHLD coalesces while pending. */
    }
    struct kernel_action action=child_action;
    if(action.handler<=1)return;
    if(action.flags&SA_RESETHAND){child_action=(struct kernel_action){0};child_install();}
    proxy_sigmask_t old=0,mask;
    RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,0,&old,8,0,0);
    mask=(*arch_signal_mask((ucontext_t *)context)|action.mask|
          ((action.flags&SA_NODEFER)?0:CHLD_BIT))&~SIGSYS_BIT;
    RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&mask,0,8,0,0);
    if(action.flags&SA_SIGINFO)((void (*)(int,siginfo_t *,void *))action.handler)(sig,info,context);
    else ((void (*)(int))action.handler)(sig);
    RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&old,0,8,0,0);
}
/* The forwarding envelope is private; signalfd consumers see the original
 * kernel CLD_* code, pid, uid, status and CPU times, never SI_QUEUE. */
static void child_read_result(void *buffer,long result) {
    for(long off=0;off+(long)sizeof(struct signalfd_siginfo)<=result;off+=sizeof(struct signalfd_siginfo)) {
        struct signalfd_siginfo *s=(void *)((char *)buffer+off);
        if(s->ssi_signo!=SIGCHLD || s->ssi_code!=SI_QUEUE || s->ssi_errno!=CHLD_FORWARD_TAG)continue;
        siginfo_t info={0};info.si_signo=SIGCHLD;info.si_code=SI_QUEUE;info.si_errno=s->ssi_errno;
        info.si_value.sival_ptr=(void *)(uintptr_t)s->ssi_ptr;
        if(!child_unwrap(&info))continue;
        *s=(struct signalfd_siginfo){.ssi_signo=SIGCHLD,.ssi_errno=info.si_errno,.ssi_code=info.si_code};
        if(info.si_code==SI_TIMER){s->ssi_tid=info.si_timerid;s->ssi_overrun=info.si_overrun;}
        else {s->ssi_pid=info.si_pid;s->ssi_uid=info.si_uid;}
        if(info.si_code>0){s->ssi_status=info.si_status;s->ssi_utime=info.si_utime;s->ssi_stime=info.si_stime;}
        else if(info.si_code==SI_QUEUE || info.si_code==SI_MESGQ || info.si_code==SI_TIMER){
            s->ssi_int=info.si_value.sival_int;s->ssi_ptr=(uintptr_t)info.si_value.sival_ptr;
        }
    }
}
#endif
