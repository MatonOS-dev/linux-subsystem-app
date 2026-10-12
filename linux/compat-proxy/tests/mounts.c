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
    CHECK(argc==2);CHECK(unshare(CLONE_NEWNS|CLONE_NEWUSER)==0);
    CHECK(mount(argv[1],"/virtual-v3",NULL,MS_BIND,NULL)==0);
    int fd=open("/virtual-v3/fixture",O_RDONLY);CHECK(fd>=0);
    char buf[16]={0};CHECK(read(fd,buf,sizeof(buf)-1)==9);CHECK(!strcmp(buf,"mount-ok\n"));close(fd);
    puts("PASS virtual bind mount under copied Android filter");return 0;
}
