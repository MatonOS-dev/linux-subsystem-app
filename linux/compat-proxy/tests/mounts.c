#define _GNU_SOURCE
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %d %s errno=%d\n",__LINE__,#c,errno);return 1;}}while(0)
int main(int argc,char **argv) {
    if(argc==3){CHECK(mount("tmpfs","/after-exec","tmpfs",0,NULL)==0);puts("PASS inherited tmpfs ownership survives exec beyond private reserve");return 0;}
    CHECK(argc==2);CHECK(unshare(CLONE_NEWNS|CLONE_NEWUSER)==0);
    CHECK(mount(argv[1],"/virtual-v3",NULL,MS_BIND,NULL)==0);
    int fd=open("/virtual-v3/fixture",O_RDONLY);CHECK(fd>=0);
    char buf[16]={0};CHECK(read(fd,buf,sizeof(buf)-1)==9);CHECK(!strcmp(buf,"mount-ok\n"));close(fd);
    /* Inherit more tmpfs ownership locks than the private reserve, then exec
     * and allocate again: portal-launched bwrap inherits the outer locks. */
    for(int i=0;i<64;i++){char target[64];snprintf(target,sizeof(target),"/tmpfs-%d",i);CHECK(mount("tmpfs",target,"tmpfs",0,NULL)==0);}
    char *child[]={argv[0],argv[1],"child",NULL};execv(argv[0],child);CHECK(0);return 1;
}
