#define _GNU_SOURCE
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s errno=%d\n",__LINE__,#x,errno);return 1;}}while(0)
int main(void){
 int fd=open("source",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
 CHECK(write(fd,"pivot",5)==5);CHECK(link("source","target")==0);
 struct stat a,b;CHECK(fstat(fd,&a)==0 && stat("target",&b)==0);
 CHECK(a.st_ino==b.st_ino && a.st_nlink==2 && b.st_nlink==2);
 close(fd);puts("PASS link-store lookup after bwrap pivot_root on 0x10d30 handler stack");return 0;
}
