/* SPDX-License-Identifier: Apache-2.0
 * A connected DNS socket, dup alias and epoll watch survive two class changes. */
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
static const unsigned char query[]={0x12,0x34,1,0,0,1,0,0,0,0,0,0,1,'x',0,0,1,0,1};
static void number(char *out,int n){char b[20];unsigned k=0,i=0;do{b[k++]=n%10+'0';n/=10;}while(n);while(k)out[i++]=b[--k];out[i]=0;}
int main(int argc,char **argv,char **env){
 if(argc==3){
  struct sockaddr_in server={.sin_family=AF_INET,.sin_port=0x3500,.sin_addr={0x632a0a7f}};
  int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0||connect(fd,(void *)&server,sizeof(server)))return 91;
  int alias=dup(fd),epoll=epoll_create1(0);if(alias<0||epoll<0)return 92;
  struct epoll_event event={.events=EPOLLIN,.data={.u64=UINT64_C(0x1122334455667788)}};
  if(epoll_ctl(epoll,EPOLL_CTL_ADD,alias,&event)||write(fd,query,sizeof(query))!=sizeof(query))return 93;
  char a[20],b[20],c[20];number(a,fd);number(b,alias);number(c,epoll);
  char *args[]={"dns-cross-argv0",argv[2],argv[1],a,b,c,"one",0};execve(argv[1],args,env);return 94;
 }
 if(argc!=7 || strcmp(argv[0],"dns-cross-argv0"))return 95;
 int fd=atoi(argv[3]),alias=atoi(argv[4]),epoll=atoi(argv[5]);
 struct epoll_event event;
 if(epoll_wait(epoll,&event,1,2000)!=1||event.data.u64!=UINT64_C(0x1122334455667788))return 96;
 unsigned char reply[64];if(read(alias,reply,sizeof(reply))!=sizeof(query)||reply[0]!=0x12||reply[1]!=0x34||!(reply[2]&0x80))return 97;
 struct sockaddr_in peer={0};socklen_t len=sizeof(peer);
 if(getpeername(alias,(void *)&peer,&len)||peer.sin_addr.s_addr!=0x632a0a7f||peer.sin_port!=0x3500)return 98;
 if(!strcmp(argv[6],"one")){
  if(write(alias,query,sizeof(query))!=sizeof(query))return 99;
  char *args[]={argv[0],argv[2],argv[1],argv[3],argv[4],argv[5],"two",0};execve(argv[1],args,env);return 100;
 }
 close(fd);close(alias);close(epoll);
 static const char ok[]="PASS mixed-class DNS aliases, epoll and original argv0\n";
 return write(1,ok,sizeof(ok)-1)==sizeof(ok)-1?0:101;
}
