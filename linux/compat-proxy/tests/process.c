#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>
static void check(int ok,const char *what){if(!ok){perror(what);exit(1);}}
static void contents(const char *expected){
    char buffer[32]={0};int fd=open("/process-v3/fixture",O_RDONLY);
    check(fd>=0,"open virtual fixture");check(read(fd,buffer,sizeof(buffer)-1)>0,"read");close(fd);
    check(!strcmp(buffer,expected),"mount inheritance");
}
int main(int argc,char **argv){
    check(argc==3,"arguments");
    check(!mount(argv[1],"/process-v3",NULL,MS_BIND,NULL),"parent mount");
    contents("parent\n");
    pid_t child=fork();check(child>=0,"fork");
    if(!child){
        contents("parent\n");check(!unshare(CLONE_NEWNS),"child namespace");
        check(!mount(argv[2],"/process-v3",NULL,MS_BIND,NULL),"child mount");
        contents("child\n");_exit(0);
    }
    int status;check(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),"child status");
    contents("parent\n");puts("PASS child inherits virtual mounts; child namespace mutation preserves parent table");
}
