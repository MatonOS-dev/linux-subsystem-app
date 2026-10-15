#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/sendfile.h>
#include <sys/syscall.h>
#include <linux/stat.h>
#include <sys/xattr.h>
static void ro(int result,int error){assert(result==-1);assert(errno==error);}
int main(int argc,char **argv){
 if(argc>1){int fd=atoi(argv[1]);char c;assert(lseek(fd,0,SEEK_CUR)==5);assert(read(fd,&c,1)==1&&c=='5');struct stat st;assert(!fstat(fd,&st)&&st.st_size==16);puts("exec inherited slice PASS");return 0;}
 int fd=open("/packed/data",O_RDONLY);assert(fd>=0);char buf[256]={0};struct stat st;assert(!fstat(fd,&st)&&st.st_size==16&&S_ISREG(st.st_mode));
 assert(!fstatat(fd,"",&st,AT_EMPTY_PATH)&&st.st_size==16);
 struct statx sx;assert(!statx(fd,"",AT_EMPTY_PATH,STATX_BASIC_STATS,&sx)&&sx.stx_size==16);
 assert(read(fd,buf,100)==16&&!memcmp(buf,"0123456789abcdef",16));assert(!read(fd,buf,1));
 assert(lseek(fd,-3,SEEK_END)==13);assert(read(fd,buf,30)==3&&!memcmp(buf,"def",3));ro(lseek(fd,-1,SEEK_SET),EINVAL);
 assert(pread(fd,buf,100,14)==2&&!memcmp(buf,"ef",2));assert(!pread(fd,buf,1,100));
 struct iovec v[2]={{buf,2},{buf+2,20}};assert(preadv(fd,v,2,13)==3&&!memcmp(buf,"def",3));assert(preadv2(fd,v,2,14,0)==2);
 assert(lseek(fd,14,SEEK_SET)==14);assert(readv(fd,v,2)==2);
 int independent=open("/packed/data",O_RDONLY);assert(independent>=0);assert(lseek(fd,0,SEEK_CUR)==16);close(independent);
 assert(lseek(fd,3,SEEK_SET)==3);int alias=dup(fd);assert(alias>=0);assert(read(alias,buf,2)==2&&!memcmp(buf,"34",2));assert(lseek(fd,0,SEEK_CUR)==5);
 int d2=fcntl(fd,F_DUPFD,100);assert(d2>=100);assert(lseek(d2,0,SEEK_CUR)==5);assert(dup2(fd,d2)==d2);assert(dup3(fd,d2,O_CLOEXEC)==d2);assert(fcntl(d2,F_GETFD)&FD_CLOEXEC);close(d2);
 char linkbuf[256],proc[64];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",fd);int n=readlink(proc,linkbuf,sizeof(linkbuf));assert(n==12&&!memcmp(linkbuf,"/packed/data",12));snprintf(proc,sizeof(proc),"/proc/%ld/fd/%d",(long)getpid(),fd);assert(readlink(proc,linkbuf,sizeof(linkbuf))==12);
 int child=fork();assert(child>=0);if(!child){char number[32];snprintf(number,sizeof(number),"%d",alias);execl("/packed/probe","probe",number,NULL);perror("exec");_exit(1);}int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));assert(lseek(fd,0,SEEK_CUR)==6);
 int clo=fcntl(fd,F_DUPFD_CLOEXEC,100);assert(clo>=100);assert(!syscall(SYS_close_range,clo,clo,0));ro(fstat(clo,&st),EBADF);
 int dir=open("/packed/nested",O_RDONLY|O_DIRECTORY);assert(dir>=0);int relative=openat(dir,"../data",O_RDONLY);assert(relative>=0);assert(read(relative,buf,100)==16);close(relative);DIR *dp=fdopendir(dir);assert(dp);int seen=0;struct dirent *de;while((de=readdir(dp)))if(!strcmp(de->d_name,"leaf"))seen++;assert(seen==1);rewinddir(dp);seen=0;while((de=readdir(dp)))if(!strcmp(de->d_name,"leaf"))seen++;assert(seen==1);closedir(dp);
 void *map=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,fd,0);assert(map!=MAP_FAILED&&!memcmp(map,"0123456789abcdef",16));munmap(map,4096);errno=0;assert(mmap(NULL,4096,PROT_WRITE,MAP_SHARED,fd,0)==MAP_FAILED&&errno==EACCES);
 int out=memfd_create("pack-out",0);assert(out>=0);off_t off=13;assert(sendfile(out,fd,&off,100)==3&&off==16);assert(pread(out,buf,100,0)==3&&!memcmp(buf,"def",3));assert(lseek(fd,0,SEEK_CUR)==6);
 off64_t a=12,b=0;assert(copy_file_range(fd,&a,out,&b,100,0)==4&&a==16&&b==4);assert(pread(out,buf,100,0)==4&&!memcmp(buf,"cdef",4));
 int pipefd[2];assert(!pipe(pipefd));a=14;assert(splice(fd,&a,pipefd[1],NULL,100,0)==2&&a==16);assert(read(pipefd[0],buf,20)==2&&!memcmp(buf,"ef",2));close(pipefd[0]);close(pipefd[1]);close(out);
 ro(write(fd,"x",1),EBADF);ro(ftruncate(fd,0),EBADF);ro(fallocate(fd,0,0,1),EBADF);
 ro(open("/packed/new",O_CREAT|O_WRONLY,0600),EROFS);ro(unlink("/packed/data"),EROFS);ro(rename("/packed/data","/packed/new"),EROFS);ro(mkdir("/packed/new",0700),EROFS);ro(rmdir("/packed/nested"),EROFS);ro(symlink("data","/packed/new"),EROFS);ro(link("/packed/data","/packed/new"),EROFS);ro(chmod("/packed/data",0600),EROFS);ro(chown("/packed/data",0,0),EROFS);ro(utimensat(AT_FDCWD,"/packed/data",NULL,0),EROFS);ro(setxattr("/packed/data","user.test","x",1,0),EROFS);
 close(alias);close(fd);puts("pack fd/path/vector/transfer/mmap/fork/exec/EROFS PASS");return 0;
}
