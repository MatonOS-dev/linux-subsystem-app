#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
int main(int argc,char **argv){
    assert(argc==2);
    if(!strcmp(argv[1],"start")){
        char *args[]={"relative-argv0","check",NULL};
        execve("./image",args,environ);perror("relative exec");return 1;
    }
    char exe[PATH_MAX];ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
    assert(n>0);exe[n]=0;
    assert(exe[0]=='/' && n>=6 && !strcmp(exe+n-6,"/image"));
    if(!strcmp(argv[1],"check")){
        assert(!strcmp((char *)getauxval(AT_EXECFN),"./image"));
        assert(!strcmp(argv[0],"relative-argv0"));
        assert(!chdir("/"));
        char after[PATH_MAX];ssize_t m=readlink("/proc/self/exe",after,sizeof(after)-1);
        assert(m==n && !memcmp(after,exe,(size_t)n));
        /* CEF's subprocess launch shape: absolute proc exe, independent cwd. */
        pid_t child=fork();assert(child>=0);
        if(!child){char *args[]={exe,"child",NULL};execve(exe,args,environ);_exit(3);}
        int status;assert(waitpid(child,&status,0)==child && status==0);
    }
    puts("PASS relative exec has stable absolute proc exe and executable child");
    return 0;
}
