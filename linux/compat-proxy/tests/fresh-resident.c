#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(int ok,const char *what){if(!ok){perror(what);_exit(90);}}
int main(int argc,char **argv){
    if(argc==2 && !strcmp(argv[1],"child")){
        char exe[PATH_MAX];
        check(!strcmp(getenv("FRESH_TEST_ENV")?:"","preserved"),"environment");
        check(getenv("FRESH_TEST_PARENT_PID") && atoi(getenv("FRESH_TEST_PARENT_PID"))==getpid(),"adopted pid");
        check(!strcmp(argv[0],"fresh-payload") && !strcmp(argv[1],"child"),"argv");
        check(fcntl(90,F_GETFD)>=0,"inherited fd");
        check(!strcmp(getcwd(exe,sizeof(exe)),"/"),"cwd");
        check(readlink("/proc/self/exe",exe,sizeof(exe)-1)>0,"exe");
        check(strstr(exe,"fresh-payload")!=NULL,"virtual exe");
        sigset_t mask;check(!sigprocmask(SIG_SETMASK,NULL,&mask) && sigismember(&mask,SIGUSR1),"signal mask");
        struct sigaction action;check(!sigaction(SIGUSR2,NULL,&action) && action.sa_handler==SIG_IGN,"ignored signal");
        struct rlimit limit;check(!getrlimit(RLIMIT_CORE,&limit) && limit.rlim_cur==0,"rlimit");
        int persona=personality(0xffffffffUL);if(persona>=0)check(persona==ADDR_NO_RANDOMIZE,"persona");
        puts("PASS fresh resident payload state");return 0;
    }
    check(argc==2,"mode");
    int marked=!strcmp(argv[1],"marked");check(marked||!strcmp(argv[1],"plain"),"mode value");
    if(marked)check(open("/dev/binderfs/binder",O_RDONLY)>=0,"binder marker open");
    int fd=open("/dev/null",O_RDONLY);check(fd>=0,"open fd");
    check(dup2(fd,90)==90,"dup fd");if(fd!=90)close(fd);
    check(!chdir("/"),"chdir");
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);check(!sigprocmask(SIG_BLOCK,&mask,NULL),"block signal");
    struct sigaction action={.sa_handler=SIG_IGN};sigemptyset(&action.sa_mask);check(!sigaction(SIGUSR2,&action,NULL),"ignore signal");
    struct rlimit limit={0,0};check(!setrlimit(RLIMIT_CORE,&limit),"set rlimit");
    check(personality(ADDR_NO_RANDOMIZE)>=0,"set persona");
    pid_t child=fork();check(child>=0,"fork");
    if(!child){char pid_env[64];snprintf(pid_env,sizeof(pid_env),"FRESH_TEST_PARENT_PID=%ld",(long)getpid());char *args[]={"fresh-payload","child",NULL};char *env[]={"FRESH_TEST_ENV=preserved",pid_env,NULL};execve(argv[0],args,env);_exit(91);}
    int status;check(waitpid(child,&status,0)==child,"waitpid");
    check(WIFEXITED(status)&&WEXITSTATUS(status)==0,"child result");return 0;
}
