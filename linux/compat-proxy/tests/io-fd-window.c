#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static volatile int ready,go;
static int alias_fd;
static void empty_inotify(int fd){char c;errno=0;CHECK(read(fd,&c,1)==-1 && errno==EAGAIN);}
static void *reader(void *unused){
 (void)unused;__atomic_store_n(&ready,1,__ATOMIC_RELEASE);
 while(!__atomic_load_n(&go,__ATOMIC_ACQUIRE))sched_yield();
 empty_inotify(alias_fd);return NULL;
}
int main(void){
 struct rlimit lim;CHECK(!getrlimit(RLIMIT_NOFILE,&lim));
 int i=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);CHECK(i>=0);CHECK(i>=512 && i<1024);
 CHECK(fcntl(i,F_GETFD)&FD_CLOEXEC);CHECK(fcntl(i,F_GETFL)&O_NONBLOCK);
 empty_inotify(i);
 /* Install the alias route while another thread is already alive. */
 pthread_t thread;CHECK(!pthread_create(&thread,NULL,reader,NULL));
 while(!__atomic_load_n(&ready,__ATOMIC_ACQUIRE))sched_yield();
 CHECK(dup2(i,77)==77);alias_fd=77;__atomic_store_n(&go,1,__ATOMIC_RELEASE);
 CHECK(!pthread_join(thread,NULL));empty_inotify(77);
 CHECK(dup3(77,78,O_CLOEXEC)==78);CHECK(fcntl(78,F_GETFD)&FD_CLOEXEC);empty_inotify(78);
 int d=fcntl(78,F_DUPFD_CLOEXEC,79);CHECK(d>=79);empty_inotify(d);
 int freefd=open("/dev/null",O_RDONLY);CHECK(freefd>=0);CHECK(!close(freefd));
 int lowest=dup(78);CHECK(lowest==freefd);empty_inotify(lowest);
 CHECK(dup2(78,i+32)==i+32);empty_inotify(i+32);
 CHECK(!close(i));CHECK(!close(77));CHECK(!close(78));CHECK(!close(d));CHECK(!close(lowest));CHECK(!close(i+32));
 /* Reuse a permanent alias number for an ordinary fd: semantics stay native,
  * although this exceptional number must still enter the handler. */
 int null=open("/dev/null",O_RDONLY);CHECK(null>=0);CHECK(dup2(null,77)==77);
 char c;CHECK(read(77,&c,1)==0);CHECK(!close(77));CHECK(!close(null));
 sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);CHECK(!pthread_sigmask(SIG_BLOCK,&mask,NULL));
 int s=signalfd(-1,&mask,SFD_NONBLOCK|SFD_CLOEXEC);CHECK(s>=512 && s<1024);
 CHECK(dup2(s,80)==80);CHECK(!close(s));CHECK(!kill(getpid(),SIGUSR1));
 struct signalfd_siginfo info;struct iovec vec[]={{&info,13},{(char *)&info+13,sizeof(info)-13}};
 CHECK(readv(80,vec,2)==sizeof(info));CHECK(info.ssi_signo==SIGUSR1);CHECK(!close(80));
 /* SCM_RIGHTS chooses a low fd without going through dup dispatch. */
 int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_DGRAM,0,pair));
 i=inotify_init1(IN_NONBLOCK);CHECK(i>=0);
 char control[CMSG_SPACE(sizeof(int))]={0};struct iovec v={&c,1};
 struct msghdr m={.msg_iov=&v,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
 struct cmsghdr *h=CMSG_FIRSTHDR(&m);h->cmsg_level=SOL_SOCKET;h->cmsg_type=SCM_RIGHTS;h->cmsg_len=CMSG_LEN(sizeof(int));
 *(int *)CMSG_DATA(h)=i;CHECK(sendmsg(pair[0],&m,0)==1);
 CHECK(recvmsg(pair[1],&m,0)==1);int received=*(int *)CMSG_DATA(CMSG_FIRSTHDR(&m));
 CHECK(received>=0);empty_inotify(received);CHECK(!close(received));
 *(int *)CMSG_DATA(CMSG_FIRSTHDR(&m))=i;m.msg_controllen=sizeof(control);CHECK(sendmsg(pair[0],&m,0)==1);
 struct mmsghdr mm={.msg_hdr=m};CHECK(recvmmsg(pair[1],&mm,1,MSG_DONTWAIT,NULL)==1);
 received=*(int *)CMSG_DATA(CMSG_FIRSTHDR(&mm.msg_hdr));empty_inotify(received);CHECK(!close(received));
 CHECK(!close(i));CHECK(!close(pair[0]));CHECK(!close(pair[1]));
 /* A full optimization window falls back to a low exact alias. */
 null=open("/dev/null",O_RDONLY);CHECK(null>=0);
 for(int fd=512;fd<1024;fd++)CHECK(dup2(null,fd)==fd);
 i=inotify_init1(IN_NONBLOCK);CHECK(i>=0 && i<512);empty_inotify(i);CHECK(!close(i));
 for(int fd=512;fd<1024;fd++)CHECK(!close(fd));
 CHECK(!close(null));
 /* Lowering RLIMIT must not invent EMFILE just because the I/O window is
  * above the new limit; the kernel's low allocation remains usable. */
 pid_t child=fork();CHECK(child>=0);
 if(!child){struct rlimit small=lim;small.rlim_cur=128;CHECK(!setrlimit(RLIMIT_NOFILE,&small));
     i=inotify_init1(IN_NONBLOCK);CHECK(i>=0 && i<128);empty_inotify(i);CHECK(!close(i));_exit(0);}
 int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
 CHECK(!unshare(CLONE_NEWUSER));int sys=open("/proc/sys/user/max_user_namespaces",O_WRONLY|O_CLOEXEC);CHECK(sys>=0);
 CHECK(dup2(sys,81)==81);CHECK(!close(sys));CHECK(write(81,"0\n",2)==2);
 CHECK(unshare(CLONE_NEWUSER)==-1 && errno==EPERM);CHECK(!close(81));
 puts("PASS I/O window: flags, lowest-free dup, exact aliases, threaded routing, readv, SCM_RIGHTS and sysctl");
 return 0;
}
