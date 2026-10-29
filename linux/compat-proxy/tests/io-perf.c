#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char **argv){
 if(argc!=3)return 2;
 const int loops=atoi(argv[2]);if(loops<=0)return 2;
 char names[128][4096],missing[128][4096],link[4096],buf[4096];
 for(int i=0;i<128;i++){snprintf(names[i],sizeof(names[i]),"%s/d%02d/file",argv[1],i);snprintf(missing[i],sizeof(missing[i]),"%s/d%02d/missing",argv[1],i);}
 snprintf(link,sizeof(link),"%s/link",argv[1]);
 int fd=open(names[0],O_RDWR);if(fd<0)return 3;
 const char *ops[]={"read","write","openat","statx","access","readlink","openat-miss","statx-miss","access-miss","readlink-miss"};
 for(int op=0;op<10;op++){
  double start=now();
  for(int i=0;i<loops;i++){
   const char *p=op>=6?missing[i%128]:names[i%128];long r;
   struct statx st;
   switch(op){
    case 0:r=syscall(SYS_read,fd,buf,1);if(!r){lseek(fd,0,SEEK_SET);r=syscall(SYS_read,fd,buf,1);}break;
    case 1:r=syscall(SYS_write,fd,"x",1);break;
    case 2:case 6:r=syscall(SYS_openat,AT_FDCWD,p,O_RDONLY|O_CLOEXEC,0);if(r>=0)close((int)r);break;
    case 3:case 7:r=syscall(SYS_statx,AT_FDCWD,p,0,STATX_BASIC_STATS,&st);break;
    case 4:case 8:r=syscall(SYS_faccessat2,AT_FDCWD,p,F_OK,0);break;
    default:r=syscall(SYS_readlinkat,AT_FDCWD,op==5?link:p,buf,sizeof(buf));break;
   }
   if((op<6 && r<0)||(op>=6 && (r!=-1||errno!=ENOENT))){fprintf(stderr,"%s failed: %ld errno=%d\n",ops[op],r,errno);return 4;}
  }
  printf("%s %.3f us/op\n",ops[op],(now()-start)*1e6/loops);
 }
 close(fd);return 0;
}
