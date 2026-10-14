#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <sys/epoll.h>
#define CHECK(x) do {if(!(x)){perror(#x);return 1;}}while(0)
static void *wait_before_send(void *pointer) {
 int fd=*(int *)pointer;struct pollfd p={.fd=fd,.events=POLLIN};
 return poll(&p,1,2000)==1 && (p.revents&POLLIN)?NULL:(void *)1;
}
static void *lookup(void *unused) {
 (void)unused; struct __res_state state={0};
 if(res_ninit(&state))return (void *)1;
 state.nscount=1; state.nsaddr_list[0].sin_family=AF_INET;state.nsaddr_list[0].sin_port=htons(53);
 inet_pton(AF_INET,"127.10.42.99",&state.nsaddr_list[0].sin_addr);
 unsigned char answer[512];int n=res_nquery(&state,"example.test",ns_c_in,ns_t_a,answer,sizeof(answer));res_nclose(&state);return n>0?NULL:(void *)1;
}
int main(int argc,char **argv) {
 CHECK(argc==2);
 struct sockaddr_in dest={.sin_family=AF_INET,.sin_port=htons(53)};
 CHECK(inet_pton(AF_INET,"127.10.42.99",&dest.sin_addr)==1);
 if(!strcmp(argv[1],"threads")) {
  pthread_t threads[12];for(int i=0;i<12;i++)CHECK(!pthread_create(&threads[i],NULL,lookup,NULL));
  for(int i=0;i<12;i++){void *result;CHECK(!pthread_join(threads[i],&result) && !result);}
 } else if(!strcmp(argv[1],"glibc") || !strcmp(argv[1],"tcp") || !strcmp(argv[1],"fallback")) {
  struct __res_state state={0};CHECK(res_ninit(&state)==0);
  state.nscount=1;state.nsaddr_list[0]=dest;state.retrans=1;state.retry=1;
  if(!strcmp(argv[1],"tcp"))state.options|=RES_USEVC;
  unsigned char answer[512];CHECK(res_nquery(&state,!strcmp(argv[1],"fallback")?"truncate.test":"example.test",ns_c_in,ns_t_a,answer,sizeof(answer))>0);
  res_nclose(&state);
 } else {
  int fd=socket(AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);
  struct timeval timeout={.tv_sec=2};CHECK(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0);
  unsigned char query[]={0x12,0x34,1,0,0,1,0,0,0,0,0,0,7,'e','x','a','m','p','l','e',4,'t','e','s','t',0,0,1,0,1};
  struct iovec iov={query,sizeof(query)};
  struct mmsghdr messages[2]={0};
  for(int i=0;i<2;i++)messages[i].msg_hdr=(struct msghdr){.msg_name=&dest,.msg_namelen=sizeof(dest),.msg_iov=&iov,.msg_iovlen=1};
  int ep=-1, alias=-1;pthread_t waiter;
  if(!strcmp(argv[1],"pre-poll")){CHECK(!pthread_create(&waiter,NULL,wait_before_send,&fd));usleep(50000);}
  if(!strcmp(argv[1],"epoll")) {
   ep=epoll_create1(EPOLL_CLOEXEC);CHECK(ep>=0);struct epoll_event event={.events=EPOLLIN|EPOLLET,.data.u64=0x1234};
   CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,fd,&event)==0);
  }
  if(!strcmp(argv[1],"alias")){alias=dup(fd);CHECK(alias>=0);}
  if(!strcmp(argv[1],"timeout")) {
   query[13]='t';query[14]='i';query[15]='m';query[16]='e';query[17]='o';query[18]='u';query[19]='t';
  }
  if(!strcmp(argv[1],"peer")) {
   CHECK(connect(fd,(void *)&dest,sizeof(dest))==0);
   struct sockaddr_in peer; socklen_t len=sizeof(peer);CHECK(getpeername(fd,(void *)&peer,&len)==0);
   CHECK(peer.sin_port==htons(53) && peer.sin_addr.s_addr==dest.sin_addr.s_addr);
   int alias=dup(fd);CHECK(alias>=0);len=sizeof(peer);CHECK(getpeername(alias,(void *)&peer,&len)==0 && peer.sin_port==htons(53));
   CHECK(send(alias,query,sizeof(query),0)==sizeof(query));close(alias);
  } else if(!strcmp(argv[1],"sendmsg"))CHECK(sendmsg(fd,&messages[0].msg_hdr,0)==sizeof(query));
  else if(!strcmp(argv[1],"sendmmsg"))CHECK(syscall(SYS_sendmmsg,fd,messages,2,0)==2 && messages[0].msg_len==sizeof(query) && messages[1].msg_len==sizeof(query));
  else CHECK(sendto(fd,query,sizeof(query),0,(void *)&dest,sizeof(dest))==sizeof(query));
  if(!strcmp(argv[1],"multiple")){query[1]=0x35;CHECK(sendto(fd,query,sizeof(query),0,(void *)&dest,sizeof(dest))==sizeof(query));}
  if(!strcmp(argv[1],"pre-poll")){void *result;CHECK(!pthread_join(waiter,&result) && !result);}
  if(!strcmp(argv[1],"poll")){struct pollfd p={.fd=fd,.events=POLLIN};CHECK(poll(&p,1,2000)==1 && (p.revents&POLLIN));}
  if(ep>=0){struct epoll_event event;CHECK(epoll_wait(ep,&event,1,2000)==1 && event.data.u64==0x1234 && (event.events&EPOLLIN));close(ep);}
  if(alias>=0){close(fd);fd=alias;}
  unsigned char answer[512];struct sockaddr_in source={0};socklen_t size=sizeof(source);int n;
  if(!strcmp(argv[1],"recvmsg")) {
   struct iovec v={answer,sizeof(answer)};struct msghdr m={.msg_name=&source,.msg_namelen=size,.msg_iov=&v,.msg_iovlen=1};
   n=recvmsg(fd,&m,0);size=m.msg_namelen;
  } else n=recvfrom(fd,answer,sizeof(answer),0,(void *)&source,&size);
  if(!strcmp(argv[1],"timeout")){CHECK(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK));}
  else {CHECK(n>0);CHECK(answer[0]==0x12 && (answer[1]==0x34 || (!strcmp(argv[1],"multiple") && answer[1]==0x35)) && (answer[2]&0x80));CHECK(size==sizeof(source) && source.sin_family==AF_INET && source.sin_port==htons(53) && source.sin_addr.s_addr==dest.sin_addr.s_addr);}
  if(!strcmp(argv[1],"multiple")){unsigned first=answer[1];CHECK(recv(fd,answer,sizeof(answer),0)>0 && answer[0]==0x12 && answer[1]!=first && (answer[1]==0x34 || answer[1]==0x35));}
  close(fd);
 }
 puts("PASS DNS lookup/endpoint translation");return 0;
}
