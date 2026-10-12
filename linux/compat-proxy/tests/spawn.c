#define _GNU_SOURCE
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
extern char **environ;
int main(void) {
    char *args[]={"/bin/true",NULL};pid_t pid;
    if(posix_spawn(&pid,args[0],NULL,NULL,args,environ)) return 1;
    int status;if(waitpid(pid,&status,0)!=pid || !WIFEXITED(status) || WEXITSTATUS(status)) return 2;
    pid=vfork();if(pid<0) return 3;
    if(!pid) {execve(args[0],args,environ);_exit(4);}
    if(waitpid(pid,&status,0)!=pid || !WIFEXITED(status) || WEXITSTATUS(status)) return 5;
    puts("PASS posix_spawn and vfork -> resident exec");return 0;
}
