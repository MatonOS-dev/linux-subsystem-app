/* SPDX-License-Identifier: Apache-2.0
 * Original freestanding entry linked to the host's compat glibc. The host
 * need not have multilib development packages or 32-bit libc startup files. */
#include <stdint.h>
#include <stddef.h>
#include <poll.h>
extern long write(int,const void *,unsigned);
extern int open(const char *,int,...);
extern long read(int,void *,unsigned);
extern int close(int);
extern int pthread_create(unsigned long *,const void *,void *(*)(void *),void *);
extern int pthread_join(unsigned long,void **);
#include <signal.h>
extern int raise(int);
extern int *__errno_location(void);
extern int pipe(int *),usleep(unsigned),execve(const char *,char *const *,char *const *);
extern long syscall(long,...);
static int fds[2];
static void *writer(void *unused){(void)unused;usleep(10000);char c='p';return write(fds[1],&c,1)==1?0:(void *)1;}

static volatile int caught;
static void on_signal(int sig){caught=sig;}
static void *worker(void *unused){(void)unused;*__errno_location()=71;int fd=open("/proc/self/status",0);char b[64];if(fd<0 || read(fd,b,sizeof(b))<=0)return (void *)1;close(fd);return *__errno_location()==71?0:(void *)2;}
int main(int argc,char **argv,char **env){
(void)argc;
 *__errno_location()=72;
 for(int i=0;i<12;i++){unsigned long thread;void *ret=(void *)3;
  if(pthread_create(&thread,0,worker,0) || pthread_join(thread,&ret) || ret)return 11;
 }
 if(*__errno_location()!=72)return 12;
 if(pipe(fds))return 14;
 unsigned long thread;if(pthread_create(&thread,0,writer,0))return 15;
 struct pollfd p={fds[0],POLLIN,0};if(poll(&p,1,-1)!=1)return 16;
 void *ret;if(pthread_join(thread,&ret) || ret)return 17;close(fds[0]);close(fds[1]);
 int dir=open("/proc/self/fd",0x10000);char dents[4096];if(dir<0 || syscall(220,dir,dents,sizeof(dents))<=0)return 18;close(dir);
 if(argc==1){char *args[]={argv[0],"exec-again",0};execve(argv[0],args,env);return 19;}
 signal(10,on_signal);if(raise(10) || caught!=10)return 13;
 static const char ok[]="PASS i386 dynamic glibc TLS, pthreads, futex and signals\n";write(1,ok,sizeof(ok)-1);return 0;
}
