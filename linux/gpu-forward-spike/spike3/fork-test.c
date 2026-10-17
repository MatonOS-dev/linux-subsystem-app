#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "adopt.h"
extern char **environ;
extern void thunk_init(void),adopt_signal_gate(int);
extern long forwarded_identity(uintptr_t *,int);
static struct adopt_api *api;
static _Thread_local int marker;
static _Atomic int stop,ready,signals_seen,reader_tid;
static int pipe_fds[2],reader_errno;
static unsigned app_prepare,app_parent,app_child;
static void prepare(void) {assert(marker==91);app_prepare++;}
static void parent(void) {assert(marker==91);app_parent++;}
static void child(void) {assert(marker==91);app_child++;}
void app_signal(int sig) {assert(sig==SIGUSR1 && marker==91);atomic_fetch_add(&signals_seen,1);}
static void identity(void) {
    uintptr_t p;
    assert(forwarded_identity(&p,1)==syscall(SYS_gettid));
    struct adopt_thread *t=api->current();
    assert(t && atomic_load(&t->tid)==syscall(SYS_gettid));
}
static void *background(void *arg) {
    (void)arg; marker=91; atomic_fetch_add(&ready,1);
    while(!atomic_load(&stop)) {
        uintptr_t p; assert(forwarded_identity(&p,0)==syscall(SYS_gettid));
        volatile char *memory=malloc(4096); assert(memory);memory[4095]=7;
        assert(memory[4095]==7);free((void *)memory);
    }
    return NULL;
}
static void *child_thread(void *arg) {
    marker=91; identity();
    volatile char *p=malloc(65536); assert(p);p[65535]=42;
    assert(p[65535]==42);free((void *)p);
    return arg;
}
static void child_checks(void) {
    assert(marker==91 && api->live()==1);
    struct adopt_thread *t=api->current();
    assert(t && t->child_tid && *(int *)t->child_tid==syscall(SYS_gettid));
    identity();
    pthread_t threads[4];
    for(uintptr_t i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,child_thread,(void *)i));
    for(uintptr_t i=0;i<4;i++) {void *v;assert(!pthread_join(threads[i],&v) && (uintptr_t)v==i);}
}
static void wait_status(pid_t pid,int code) {
    int status;assert(waitpid(pid,&status,0)==pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==code);
}
static void fork_case(int exec) {
    unsigned rp=atomic_load(&api->fork_prepares),rc=atomic_load(&api->fork_children);
    unsigned rr=atomic_load(&api->fork_parents);
    unsigned ap=app_prepare,ac=app_child,ar=app_parent;
    pid_t pid=fork();assert(pid>=0);
    if(!pid) {
        assert(atomic_load(&api->fork_prepares)==rp+1 && atomic_load(&api->fork_children)==rc+1);
        assert(atomic_load(&api->fork_parents)==rr);
        assert(app_prepare==ap+1 && app_child==ac+1 && app_parent==ar);
        if(exec) {
            assert(api->live()==1);
            execl("/bin/sh","sh","-c","exit 23",(char *)NULL);_exit(127);
        }
        child_checks();
        /* Repeat fork after the child has created and joined adopted threads. */
        pid_t grandchild=fork();assert(grandchild>=0);
        if(!grandchild) {child_checks();_exit(29);}
        wait_status(grandchild,29);
        _exit(31);
    }
    wait_status(pid,exec?23:31);
    assert(atomic_load(&api->fork_prepares)==rp+1 && atomic_load(&api->fork_parents)==rr+1);
    assert(atomic_load(&api->fork_children)==rc);
    assert(app_prepare==ap+1 && app_parent==ar+1 && app_child==ac);
}
static void *reader(void *arg) {
    (void)arg;marker=91;atomic_store(&reader_tid,syscall(SYS_gettid));
    char byte;ssize_t n=read(pipe_fds[0],&byte,1);reader_errno=errno;
    return (void *)(intptr_t)n;
}
static void blocking_signal(int restart) {
    struct sigaction sa={0};sa.sa_handler=adopt_signal_gate;
    sa.sa_flags=restart?SA_RESTART:0;sigemptyset(&sa.sa_mask);
    assert(!sigaction(SIGUSR1,&sa,NULL));assert(!pipe(pipe_fds));
    atomic_store(&reader_tid,0);pthread_t t;assert(!pthread_create(&t,NULL,reader,NULL));
    int blocked=0;
    for(unsigned i=0;i<5000 && !blocked;i++) {
        int tid=atomic_load(&reader_tid);
        if(tid) {
            char path[128];snprintf(path,sizeof(path),"/proc/self/task/%d/syscall",tid);
            FILE *file=fopen(path,"r");assert(file);long nr=-1;
            if(fscanf(file,"%ld",&nr)==1 && nr==SYS_read) blocked=1;
            assert(!fclose(file));
        }
        if(!blocked) usleep(1000);
    }
    assert(blocked);
    int count=atomic_load(&signals_seen);assert(!pthread_kill(t,SIGUSR1));
    while(atomic_load(&signals_seen)==count) sched_yield();
    if(restart) assert(write(pipe_fds[1],"x",1)==1);
    void *value;assert(!pthread_join(t,&value));
    assert((intptr_t)value==(restart?1:-1));if(!restart) assert(reader_errno==EINTR);
    assert(!close(pipe_fds[0]) && !close(pipe_fds[1]));
}
static void *nonmain(void *arg) {
    marker=91;fork_case(0);fork_case(1);return arg;
}
int main(void) {
    thunk_init();marker=91;
    api=(void *)(uintptr_t)strtoull(getenv("MATON_THREAD_ADOPT"),NULL,16);
    struct sigaction sa={0};sa.sa_handler=adopt_signal_gate;sigemptyset(&sa.sa_mask);
    assert(!sigaction(SIGUSR1,&sa,NULL));
    assert(!pthread_atfork(prepare,parent,child));
    pthread_t workers[8];
    for(unsigned i=0;i<8;i++) assert(!pthread_create(&workers[i],NULL,background,NULL));
    while(atomic_load(&ready)!=8) sched_yield();
    fork_case(0);fork_case(1);
    pthread_t caller;assert(!pthread_create(&caller,NULL,nonmain,NULL));assert(!pthread_join(caller,NULL));
    unsigned prepares=atomic_load(&api->fork_prepares);
    unsigned vforks=atomic_load(&api->vforks);
    posix_spawn_file_actions_t actions;assert(!posix_spawn_file_actions_init(&actions));
    assert(!posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0));
    char *args[]={"sh","-c","exit 37",NULL};pid_t pid;
    assert(!posix_spawn(&pid,"/bin/sh",&actions,NULL,args,environ));wait_status(pid,37);
    assert(posix_spawn(&pid,"/does/not/exist",&actions,NULL,args,environ)==ENOENT);
    assert(!posix_spawn_file_actions_destroy(&actions));
    int status=system("exit 41");assert(WIFEXITED(status) && WEXITSTATUS(status)==41);
    char *vargs[]={"sh","-c","exit 43",NULL};
    pid=vfork();assert(pid>=0);
    if(!pid) {execve("/bin/sh",vargs,environ);_exit(127);}
    wait_status(pid,43);
    assert(atomic_load(&api->fork_prepares)==prepares);
    assert(atomic_load(&api->vforks)>vforks);
    atomic_store(&stop,1);
    for(unsigned i=0;i<8;i++) assert(!pthread_join(workers[i],NULL));
    identity();blocking_signal(0);blocking_signal(1);
    puts("PASS: blocking read signal interruption and SA_RESTART through syscall gateway");
    printf("PASS: fork+exec, fork child threads/thunks, nested/non-main fork, both libc atfork hooks; posix_spawn, failed spawn, system, vfork+exec; vfork retries=%u\n",atomic_load(&api->vforks));
}
