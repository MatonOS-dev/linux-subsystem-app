/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char **argv){
    if(argc==4 && !strcmp(argv[1],"after")){
        int clo=atoi(argv[2]),inherited=atoi(argv[3]);
        if(fcntl(clo,F_GETFD)!=-1 || errno!=EBADF)return 10;
        if(fcntl(inherited,F_GETFD)!=0)return 11;
        puts("PASS native F_GETFD/F_SETFD walk, payload CLOEXEC and private-fd exec");
        return 0;
    }
    if(argc!=1)return 2;
    struct rlimit lim;
    if(getrlimit(RLIMIT_NOFILE,&lim))return 3;
    int clo=open("/dev/null",O_RDONLY|O_CLOEXEC);
    int inherited=open("/dev/null",O_RDONLY);
    if(clo<0 || inherited<0 || clo==inherited)return 4;
    for(rlim_t fd=0;fd<lim.rlim_cur;fd++){
        int flags=fcntl((int)fd,F_GETFD);
        if(flags<0){if(errno==EBADF)continue;perror("F_GETFD");return 5;}
        int wanted=(fd==1 || fd==(rlim_t)inherited)?0:FD_CLOEXEC;
        if(fcntl((int)fd,F_SETFD,wanted))return 6;
    }
    if(fcntl(clo,F_GETFD)!=FD_CLOEXEC || fcntl(inherited,F_GETFD)!=0)return 7;
    char clo_text[24],inherited_text[24];
    snprintf(clo_text,sizeof(clo_text),"%d",clo);
    snprintf(inherited_text,sizeof(inherited_text),"%d",inherited);
    char *args[]={argv[0],"after",clo_text,inherited_text,NULL};
    execv(argv[0],args);
    perror("execv");return 8;
}
