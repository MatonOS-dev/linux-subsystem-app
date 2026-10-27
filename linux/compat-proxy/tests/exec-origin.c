#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>
extern char **environ;
extern int origin_fixture(void);
int main(int argc,char **argv){
    assert(origin_fixture()==42);
    if(argc==1){
        char *next[]={argv[0],"reexec",NULL};
        execve("/proc/self/exe",next,environ);
        perror("execve self");return 1;
    }
    char exe[4096];ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
    assert(n>0);exe[n]=0;
    assert(!strcmp(exe,argv[0]));
    assert(!strcmp((char *)getauxval(AT_EXECFN),argv[0]));
    puts("PASS proc self re-exec retains payload identity and RPATH $ORIGIN");
    return 0;
}
