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
#define CHECK(x) do {if(!(x)){perror(#x);return 1;}}while(0)
int main(int argc,char **argv) {
 CHECK(argc==2);
 struct sockaddr_in dest={.sin_family=AF_INET,.sin_port=htons(53)};
 CHECK(inet_pton(AF_INET,"127.10.42.99",&dest.sin_addr)==1);
 if(!strcmp(argv[1],"glibc") || !strcmp(argv[1],"tcp")) {
  struct __res_state state={0};CHECK(res_ninit(&state)==0);
  state.nscount=1;state.nsaddr_list[0]=dest;state.retrans=1;state.retry=1;
  if(!strcmp(argv[1],"tcp"))state.options|=RES_USEVC;
  unsigned char answer[512];CHECK(res_nquery(&state,"example.test",ns_c_in,ns_t_a,answer,sizeof(answer))>0);
  res_nclose(&state);
 } else {
  int fd=socket(AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);
  struct timeval timeout={.tv_sec=2};CHECK(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0);
  unsigned char query[]={0x12,0x34,1,0,0,1,0,0,0,0,0,0,7,'e','x','a','m','p','l','e',4,'t','e','s','t',0,0,1,0,1};
  struct iovec iov={query,sizeof(query)};
  struct mmsghdr messages[2]={0};
  for(int i=0;i<2;i++)messages[i].msg_hdr=(struct msghdr){.msg_name=&dest,.msg_namelen=sizeof(dest),.msg_iov=&iov,.msg_iovlen=1};
  if(!strcmp(argv[1],"peer")) {
   CHECK(connect(fd,(void *)&dest,sizeof(dest))==0);
   struct sockaddr_in peer; socklen_t len=sizeof(peer);CHECK(getpeername(fd,(void *)&peer,&len)==0);
   CHECK(peer.sin_port==htons(53) && peer.sin_addr.s_addr==dest.sin_addr.s_addr);
   int alias=dup(fd);CHECK(alias>=0);len=sizeof(peer);CHECK(getpeername(alias,(void *)&peer,&len)==0 && peer.sin_port==htons(53));
   CHECK(send(alias,query,sizeof(query),0)==sizeof(query));close(alias);
  } else if(!strcmp(argv[1],"sendmsg"))CHECK(sendmsg(fd,&messages[0].msg_hdr,0)==sizeof(query));
  else if(!strcmp(argv[1],"sendmmsg"))CHECK(syscall(SYS_sendmmsg,fd,messages,2,0)==2 && messages[0].msg_len==sizeof(query) && messages[1].msg_len==sizeof(query));
  else if(!strcmp(argv[1],"native")) {
   dest.sin_port=htons(5353);CHECK(connect(fd,(void *)&dest,sizeof(dest))==0);
   struct sockaddr_in direct;socklen_t direct_len=sizeof(direct);
   CHECK(getpeername(fd,(void *)&direct,&direct_len)==0 && direct.sin_port==htons(5353));
   dest.sin_addr.s_addr=htonl(0x7f0a2a64);dest.sin_port=htons(5353);
   CHECK(connect(fd,(void *)&dest,sizeof(dest))==0);
   struct sockaddr_in peer;socklen_t len=sizeof(peer);CHECK(getpeername(fd,(void *)&peer,&len)==0 && peer.sin_port==htons(5353));
   CHECK(send(fd,query,sizeof(query),0)==sizeof(query));
  } else CHECK(sendto(fd,query,sizeof(query),0,(void *)&dest,sizeof(dest))==sizeof(query));
  unsigned char answer[512];CHECK(recv(fd,answer,sizeof(answer),0)>0);CHECK(answer[0]==0x12 && answer[1]==0x34 && (answer[2]&0x80));close(fd);
 }
 puts("PASS DNS lookup/endpoint translation");return 0;
}
