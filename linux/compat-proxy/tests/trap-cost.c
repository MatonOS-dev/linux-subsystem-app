#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
extern int *__errno_location(void);
#undef errno
#define errno (*__errno_location())

static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(int argc,char **argv){
 if(argc!=5){fprintf(stderr,"usage: %s OP ITERATIONS PATH TARGET\n",argv[0]);return 2;}
 const char *op=argv[1],*path=argv[3],*target=argv[4];unsigned long n=strtoul(argv[2],0,10);volatile long sum=0;char cwd[4096],buf[128];struct stat st;
 uint64_t start=ns();
 for(unsigned long i=0;i<n;i++){
  if(!strcmp(op,"getcwd")){if(!getcwd(cwd,sizeof(cwd)))return perror("getcwd"),1;sum+=cwd[0];}
  else if(!strcmp(op,"close")){errno=0;if(close(-1)!=-1||errno!=EBADF)return 1;sum+=errno;}
  else if(!strcmp(op,"access")){if(access(path,F_OK))return perror("access"),1;}
  else if(!strcmp(op,"stat")){if(stat(path,&st))return perror("stat"),1;sum+=st.st_ino;}
  else if(!strcmp(op,"openat")){int fd=openat(AT_FDCWD,path,O_RDONLY|O_CLOEXEC);if(fd<0)return perror("openat"),1;sum+=close(fd);}
  else if(!strcmp(op,"readlink")){ssize_t r=readlink(path,buf,sizeof(buf));if(r<0)return perror("readlink"),1;sum+=r;}
  else if(!strcmp(op,"unlinkat")){if(unlinkat(AT_FDCWD,path,0))return perror("unlinkat"),1;int fd=open(path,O_CREAT|O_WRONLY|O_CLOEXEC,0600);if(fd<0)return perror("recreate"),1;close(fd);}
  else if(!strcmp(op,"linkat")){char unique[4096];snprintf(unique,sizeof(unique),"%s.%lu",target,i);if(linkat(AT_FDCWD,path,AT_FDCWD,unique,0))return perror("linkat"),1;if(unlinkat(AT_FDCWD,unique,0))return perror("unlink linked"),1;}
  else if(!strcmp(op,"mkdirat")){if(mkdirat(AT_FDCWD,path,0700)&&errno!=EEXIST)return perror("mkdirat"),1;unlinkat(AT_FDCWD,path,AT_REMOVEDIR);}
  else return fprintf(stderr,"unknown op %s\n",op),2;
 }
 uint64_t elapsed=ns()-start;printf("op=%s iterations=%lu us_per_call=%.3f checksum=%ld\n",op,n,(double)elapsed/n/1000.0,sum);return 0;
}
