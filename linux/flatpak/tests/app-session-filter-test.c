#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include "../app-session.h"

static int check(void) {
    errno=0;
    if(ioctl(-1,0xc0306201,0)!=-1 || errno!=EPERM)return 1;
    errno=0;
    if(ioctl(-1,0x6209,0)!=-1 || errno!=EPERM)return 2;
    errno=0;
    if(ioctl(-1,0x6401,0)!=-1 || errno!=EBADF)return 3;
    if(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)!=1)return 4;
    return 0;
}
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"exec"))return check();
    if(session_filter_binder())return 5;
    int rc=check();if(rc)return rc;
    pid_t child=fork();if(child<0)return 6;
    if(!child){execl(argv[0],argv[0],"exec",NULL);_exit(7);}
    int status;if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status))return 8;
    child=fork();if(child<0)return 9;
    if(!child){syscall(SYS_getpid|0x40000000);_exit(10);}
    if(waitpid(child,&status,0)!=child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGSYS)return 11;
    puts("PASS: Binder ioctl denial, ordinary ioctl allowed, NNP, fork/exec inheritance, x32 rejection");
    return 0;
}
