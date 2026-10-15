/* SPDX-License-Identifier: Apache-2.0 */
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
extern long syscall(long,...);
extern int *__errno_location(void);
#undef errno
#define errno (*__errno_location())
int main(int argc,char **argv,char **env){
    if(argc!=6)return 2;
    const int errors[]={ENOENT,ENOENT,EACCES,ENOEXEC,ENAMETOOLONG};
    int fd=open("/dev/null",O_RDONLY|O_CLOEXEC);if(fd<0)return 3;
    for(int at=0;at<2;at++)for(int i=0;i<5;i++){
        char *args[]={argv[i+1],0};
        long result=at?syscall(sizeof(long)==4?358:322,-100L,args[0],args,env,0L):execve(args[0],args,env);
        if(result!=-1 || errno!=errors[i]){
            fprintf(stderr,"exec case %d: errno %d expected %d\n",i,errno,errors[i]);return 1;
        }
    }
    if(fcntl(fd,F_GETFD)!=FD_CLOEXEC)return 4;
    close(fd);puts("PASS execve/execveat failures preserve caller, CLOEXEC fd and errno");return 0;
}
