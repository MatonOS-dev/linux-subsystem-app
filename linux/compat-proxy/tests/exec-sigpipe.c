#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc,char **argv){
    if(argc>1 && !strcmp(argv[1],"sent-signal")){signal(SIGPIPE,SIG_DFL);raise(SIGPIPE);return 19;}
    if(argc>1 && (!strcmp(argv[1],"closed-log")||!strcmp(argv[1],"closed-log-pending"))){
        signal(SIGPIPE,SIG_DFL);
        int pending=!strcmp(argv[1],"closed-log-pending");
        sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGPIPE);
        if(pending){if(sigprocmask(SIG_BLOCK,&mask,NULL)||raise(SIGPIPE))return 15;}
        if(write(1,"ready\n",6)!=6)return 10;
        char c;if(read(0,&c,1)!=1)return 11;
        if(getpid()<=0)return 12;
        struct sigaction sa;if(sigaction(SIGPIPE,NULL,&sa)||sa.sa_handler!=SIG_DFL)return 13;
        sigset_t current;if(sigprocmask(SIG_SETMASK,NULL,&current)||sigismember(&current,SIGPIPE)!=pending)return 16;
        if(sigpending(&current)||sigismember(&current,SIGPIPE)!=pending)return 17;
        if(pending){int sig;if(sigwait(&mask,&sig)||sig!=SIGPIPE)return 18;}
        return 0;
    }
    if(argc>1 && (!strcmp(argv[1],"write")||!strcmp(argv[1],"default-write"))){
        struct sigaction sa;if(sigaction(SIGPIPE,NULL,&sa)||sa.sa_handler!=SIG_IGN)return 2;
        int fd=atoi(argv[2]);errno=0;
        if(!strcmp(argv[1],"default-write"))signal(SIGPIPE,SIG_DFL);
        if(write(fd,"x",1)!=-1||errno!=EPIPE)return 3;
        return 0;
    }
    signal(SIGPIPE,SIG_IGN);
    int fds[2];if(pipe(fds))return 4;close(fds[0]);
    pid_t child=fork();if(child<0)return 5;
    if(!child){char fd[32];snprintf(fd,sizeof(fd),"%d",fds[1]);execl(argv[0],argv[0],"write",fd,NULL);_exit(6);}
    int status;
    if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status))return 7;
    child=fork();if(child<0)return 8;
    if(!child){char fd[32];snprintf(fd,sizeof(fd),"%d",fds[1]);execl(argv[0],argv[0],"default-write",fd,NULL);_exit(9);}
    close(fds[1]);if(waitpid(child,&status,0)!=child||!WIFSIGNALED(status)||WTERMSIG(status)!=SIGPIPE)return 14;
    puts("PASS SIG_IGN survives fork/exec; closed pipe returns EPIPE");return 0;
}
