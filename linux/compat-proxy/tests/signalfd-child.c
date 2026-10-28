#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){perror(#x);exit(1);}} while(0)
int main(void) {
    /* bwrap's outer monitor, without the eventfd used by --unshare-pid:
     * notification must come from SIGCHLD alone. Repeat to cover children
     * exiting before and after entry into the infinite poll. */
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGCHLD);
    struct sigaction action={.sa_handler=SIG_DFL};
    CHECK(sigaction(SIGCHLD,&action,NULL)==0);
    CHECK(sigprocmask(SIG_BLOCK,&mask,NULL)==0);
    int fd=signalfd(-1,&mask,SFD_CLOEXEC|SFD_NONBLOCK);CHECK(fd>=0);
    for(int i=0;i<20;i++) {
        pid_t child=fork();CHECK(child>=0);
        if(!child){if(i%2)usleep(10000);_exit(i);}
        struct pollfd p={.fd=fd,.events=POLLIN};
        long n;do {n=syscall(SYS_poll,&p,1,-1);}while(n<0&&errno==EINTR);
        CHECK(n==1&&(p.revents&POLLIN));
        struct signalfd_siginfo info;CHECK(read(fd,&info,sizeof(info))==sizeof(info));
        CHECK(info.ssi_signo==SIGCHLD&&info.ssi_pid==(unsigned)child);
        int status;CHECK(waitpid(child,&status,0)==child);
        CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==i);
    }
    close(fd);puts("PASS child exit wakes infinite poll/signalfd without eventfd");
    return 0;
}
