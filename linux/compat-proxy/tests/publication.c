#define _GNU_SOURCE
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s errno=%d\n",__LINE__,#x,errno);return 1;}}while(0)
int main(int argc,char **argv){
 CHECK(argc==3);CHECK(unshare(CLONE_NEWNS|CLONE_NEWUSER)==0);
 CHECK(mount(argv[1],"/pub",NULL,MS_BIND,NULL)==0);CHECK(mount(argv[2],"/other",NULL,MS_BIND,NULL)==0);
 int dir=open("/pub",O_RDONLY|O_DIRECTORY);CHECK(dir>=0);
 int fd=openat(dir,".",O_TMPFILE|O_RDWR|O_CLOEXEC,0600);CHECK(fd>=0);CHECK(write(fd,"environ",7)==7);
 char proc[64];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",fd);
 CHECK(linkat(AT_FDCWD,proc,dir,"run-environ",AT_SYMLINK_FOLLOW)==0);
 int named=open("/pub/run-environ",O_RDONLY);char buf[8]={0};CHECK(named>=0 && read(named,buf,7)==7 && !strcmp(buf,"environ"));close(named);
 int alias=dup(fd);CHECK(alias>=0);
 CHECK(linkat(alias,"",dir,"empty",AT_EMPTY_PATH)==0);close(alias);
 /* glnx replace: publish to a temporary name, then rename to run-environ. */
 int replacement=openat(dir,".",O_TMPFILE|O_RDWR,0400);CHECK(replacement>=0);
 CHECK(write(replacement,"updated",7)==7);snprintf(proc,sizeof(proc),"/proc/self/fd/%d",replacement);
 CHECK(linkat(AT_FDCWD,proc,dir,"tmp.replace",AT_SYMLINK_FOLLOW)==0);
 CHECK(renameat(dir,"tmp.replace",dir,"run-environ")==0);close(replacement);
 errno=0;CHECK(linkat(fd,"",AT_FDCWD,"/other/cross",AT_EMPTY_PATH)==-1 && errno==EXDEV);
 CHECK(syscall(SYS_renameat2,dir,"empty",dir,"new",RENAME_NOREPLACE)==0);
 errno=0;CHECK(syscall(SYS_renameat2,dir,"new",dir,"run-environ",RENAME_NOREPLACE)==-1 && errno==EEXIST);
 int different=openat(dir,"different",O_CREAT|O_TRUNC|O_WRONLY,0600);CHECK(different>=0 && write(different,"other",5)==5);close(different);
 CHECK(syscall(SYS_renameat2,dir,"different",dir,"new",RENAME_EXCHANGE)==0);
 named=openat(dir,"new",O_RDONLY);memset(buf,0,sizeof(buf));CHECK(named>=0 && read(named,buf,7)==5 && !strcmp(buf,"other"));close(named);
 errno=0;CHECK(syscall(SYS_renameat2,dir,"new",AT_FDCWD,"/other/new",RENAME_EXCHANGE)==-1 && errno==EXDEV);
 CHECK(mount(NULL,"/pub",NULL,MS_BIND|MS_REMOUNT|MS_RDONLY,NULL)==0);
 errno=0;CHECK(linkat(fd,"",dir,"readonly",AT_EMPTY_PATH)==-1 && errno==EROFS);
 errno=0;CHECK(open("/pub",O_TMPFILE|O_RDWR,0600)==-1 && errno==EROFS);
 close(fd);close(dir);puts("PASS O_TMPFILE publication, proc magic links, AT_EMPTY_PATH and renameat2 mount policy");return 0;
}
