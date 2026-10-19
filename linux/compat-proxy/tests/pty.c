#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
struct term2 {unsigned i,o,c,l;unsigned char line,cc[19];unsigned is,os;};
int main(void){
 int m=open("/dev/pts/ptmx",O_RDWR|O_NOCTTY);assert(m>=0);
 assert(!grantpt(m)&&!unlockpt(m));char *name=ptsname(m);assert(name);
 int s=open(name,O_RDWR|O_NOCTTY);assert(s>=0);
 struct term2 t;assert(!ioctl(s,0x802c542a,&t));assert(!ioctl(s,0x402c542b,&t));
 struct termios classic;assert(!tcgetattr(s,&classic)&&!tcsetattr(s,TCSANOW,&classic));
 void *bad=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(bad!=MAP_FAILED);
 for(unsigned long cmd=0x402c542b;cmd<=0x402c542d;cmd++){errno=0;assert(ioctl(s,cmd,bad)==-1&&errno==EFAULT);}
 errno=0;assert(ioctl(s,0x802c542a,bad)==-1&&errno==EFAULT);
 assert(!mprotect(bad,4096,PROT_READ));errno=0;assert(ioctl(s,0x802c542a,bad)==-1&&errno==EFAULT);
 /* First 36 bytes accessible but trailing speeds are not. */
 assert(!mprotect(bad,4096,PROT_READ|PROT_WRITE));
 void *guard=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(guard!=MAP_FAILED);
 assert(!mprotect((char *)guard+4096,4096,PROT_NONE));
 errno=0;assert(ioctl(s,0x802c542a,(char *)guard+4096-36)==-1&&errno==EFAULT);
 pid_t child=fork();assert(child>=0);
 if(!child){
  close(m);assert(setsid()>=0);assert(!ioctl(s,TIOCSCTTY,0));
  assert(!tcsetpgrp(s,getpgrp()));assert(tcgetpgrp(s)==getpgrp());
  int tty=open("/dev/tty",O_RDWR);assert(tty>=0);close(tty);
  assert(dup2(s,0)==0&&dup2(s,1)==1&&dup2(s,2)==2);close(s);
  /* Try all payload fd mutation paths; diagnostics must survive them. */
  close(256);errno=0;assert(dup2(0,256)==-1&&errno==EBADF);
  /* The native CLOEXEC fast path exposes the real kernel flag even for a
   * proxy-private descriptor; resident exec must preserve that descriptor. */
  assert(!fcntl(256,F_SETFD,FD_CLOEXEC));
  assert(!syscall(SYS_close_range,3,~0U,4));
  long r=syscall(SYS_close_range,3,~0U,0);assert(r==0||(r==-1&&errno==ENOSYS));
  execl("/bin/sh","sh","-c","echo ok",NULL);_exit(127);
 }
 close(s);char out[4096];size_t n=0;ssize_t k;
 while((k=read(m,out+n,sizeof(out)-1-n))>0){n+=(size_t)k;assert(n<sizeof(out)-1);}
 out[n]=0;int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 assert(strstr(out,"ok"));assert(!strstr(out,"v3 trap"));close(m);
 struct passwd *pw=getpwuid(getuid());printf("PASS pty shell and EFAULT; host passwd shell=%s\n",pw?pw->pw_shell:"missing");
}
