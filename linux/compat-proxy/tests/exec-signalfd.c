/* A threaded Flatpak execs bwrap, whose only wakeup is child SIGCHLD.
 * Native: passes. Proxy before SIGCHLD routing: timeout, because the
 * surviving unblocked sibling consumes the process-directed notification.
 * Run with an external timeout; this deliberately exercises infinite poll. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/uio.h>
#define main signalfd_main
#include "signalfd-child.c"
#undef main
static char *executable;
static void *exec_worker(void *unused){
    (void)unused;char *args[]={executable,"monitor",NULL};
    execv(executable,args);_exit(97);
}
static void *worker(void *unused){(void)unused;for(;;)pause();return NULL;}
static volatile sig_atomic_t notified,notified_tid;
static void child_handler(int sig,siginfo_t *info,void *context){
    (void)context;
    if(sig!=SIGCHLD || info->si_code!=CLD_EXITED || info->si_status!=29)_exit(96);
    notified_tid=syscall(SYS_gettid);notified=1;
}
static int ready_pipe[2];
static void *signalfd_waiter(void *unused){
    (void)unused;
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGCHLD);
    CHECK(!pthread_sigmask(SIG_BLOCK,&mask,NULL));
    int fd=signalfd(-1,&mask,SFD_CLOEXEC);CHECK(fd>=0);
    CHECK(write(ready_pipe[1],"r",1)==1);
    struct signalfd_siginfo info;CHECK(read(fd,&info,sizeof(info))==sizeof(info));
    CHECK(info.ssi_signo==SIGCHLD && info.ssi_code==CLD_EXITED && info.ssi_status==23);
    CHECK(!close(fd));return (void *)(uintptr_t)info.ssi_pid;
}
static int secondary_monitor(void){
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGCHLD);
    CHECK(!sigprocmask(SIG_BLOCK,&mask,NULL));
    CHECK(!pipe(ready_pipe));pthread_t thread;
    CHECK(!pthread_create(&thread,NULL,signalfd_waiter,NULL));
    char ready;CHECK(read(ready_pipe[0],&ready,1)==1);
    pid_t child=fork();CHECK(child>=0);if(!child)_exit(23);
    void *result;CHECK(!pthread_join(thread,&result));CHECK((pid_t)(uintptr_t)result==child);
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==23);
    CHECK(!close(ready_pipe[0]));CHECK(!close(ready_pipe[1]));
    puts("PASS SIGCHLD routes to secondary signalfd waiter and leaves main wait status");return 0;
}
static int other_monitor(const char *mode){
    if(!strcmp(mode,"secondary"))return secondary_monitor();
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGCHLD);
    CHECK(!sigprocmask(SIG_BLOCK,&mask,NULL));
    struct sigaction current;
    CHECK(!sigaction(SIGCHLD,NULL,&current) && current.sa_handler==SIG_DFL);
    if(!strcmp(mode,"ignore") || !strcmp(mode,"nocldwait")){
        struct sigaction action={.sa_handler=!strcmp(mode,"ignore")?SIG_IGN:SIG_DFL,
                                 .sa_flags=!strcmp(mode,"ignore")?0:SA_NOCLDWAIT};
        CHECK(!sigaction(SIGCHLD,&action,NULL));
        CHECK(!sigaction(SIGCHLD,NULL,&current) && current.sa_handler==action.sa_handler);
        CHECK((current.sa_flags&SA_NOCLDWAIT)==(action.sa_flags&SA_NOCLDWAIT));
        pid_t child=fork();CHECK(child>=0);if(!child)_exit(29);
        CHECK(waitpid(child,NULL,0)==-1 && errno==ECHILD);
    }else if(!strcmp(mode,"handler")){
        struct sigaction action={.sa_sigaction=child_handler,.sa_flags=SA_SIGINFO};
        CHECK(!sigaction(SIGCHLD,&action,NULL));
        pid_t child=fork();CHECK(child>=0);if(!child)_exit(29);
        sigset_t empty;sigemptyset(&empty);while(!notified)sigsuspend(&empty);
        CHECK(notified_tid==syscall(SYS_gettid));
        int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==29);
    }else for(int i=0;i<20;i++){
        int fd=-1;
        if(!strcmp(mode,"readv")){
            int original=signalfd(-1,&mask,SFD_CLOEXEC);CHECK(original>=0);
            fd=dup(original);CHECK(fd>=0);CHECK(!close(original));
        }
        pid_t child=fork();CHECK(child>=0);if(!child)_exit(i);
        if(fd>=0){
            struct signalfd_siginfo info;
            struct iovec iov[]={{&info,13},{(char *)&info+13,sizeof(info)-13}};
            CHECK(readv(fd,iov,2)==sizeof(info));
            CHECK(info.ssi_signo==SIGCHLD && info.ssi_code==CLD_EXITED && info.ssi_status==i && info.ssi_pid==(unsigned)child);
            CHECK(!close(fd));
        }else{
            siginfo_t info;CHECK(sigwaitinfo(&mask,&info)==SIGCHLD);
            CHECK(info.si_code==CLD_EXITED && info.si_pid==child && info.si_status==i);
        }
        int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==i);
    }
    printf("PASS threaded exec SIGCHLD %s metadata and wait status\n",mode);return 0;
}
int main(int argc,char **argv){
    if(argc==2 && !strcmp(argv[1],"nonleader")){
        executable=argv[0];pthread_t thread;
        CHECK(!pthread_create(&thread,NULL,exec_worker,NULL));
        for(;;)pause();
    }
    if(argc==1 || (argc==2 && !strncmp(argv[1],"launch-",7))){
        pthread_t thread;CHECK(!pthread_create(&thread,NULL,worker,NULL));
        char *args[]={argv[0],argc==1?"monitor":argv[1]+7,NULL};execv(argv[0],args);CHECK(0);
    }
    if(argc==2 && strcmp(argv[1],"monitor"))return other_monitor(argv[1]);
    return signalfd_main();
}
