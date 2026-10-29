#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
static volatile sig_atomic_t seen;
static void changed(int sig) { (void)sig; seen=1; }
int main(void) {
    struct sigaction action={.sa_handler=changed};
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGCHLD,&action,0))return 1;
    for(int signaled=0;signaled<2;signaled++) {
        sigset_t mask,old;
        sigemptyset(&mask);sigaddset(&mask,SIGCHLD);
        if(sigprocmask(SIG_BLOCK,&mask,&old))return 2;
        seen=0;
        pid_t child=fork();if(child<0)return 3;
        if(!child){if(signaled)kill(getpid(),SIGTERM);_exit(23);}
        while(!seen)sigsuspend(&old);
        if(sigprocmask(SIG_SETMASK,&old,0))return 4;
        int status;pid_t got;
        do{got=waitpid(child,&status,0);}while(got<0 && errno==EINTR);
        if(got!=child)return 5;
        if(signaled?(!WIFSIGNALED(status)||WTERMSIG(status)!=SIGTERM):
                    (!WIFEXITED(status)||WEXITSTATUS(status)!=23))return 6;
        printf("child=%ld code=%d status=%d\n",(long)child,
               signaled?CLD_KILLED:CLD_EXITED,signaled?SIGTERM:23);
    }
    return 0;
}
