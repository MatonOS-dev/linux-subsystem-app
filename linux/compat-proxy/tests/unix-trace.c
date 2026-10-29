#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
int main(void){
 int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return 1;
 struct sockaddr_un a;memset(&a,0xa5,sizeof(a));a.sun_family=AF_UNIX;
 strcpy(a.sun_path,"/tmp/maton-wsi-trace");
 (void)connect(fd,(struct sockaddr*)&a,sizeof(a));
 const unsigned char abstract[]={0,'w','s','i',0,0xff};
 memcpy(a.sun_path,abstract,sizeof(abstract));
 (void)connect(fd,(struct sockaddr*)&a,offsetof(struct sockaddr_un,sun_path)+sizeof(abstract));
 close(fd);
 int pair[2];if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair))return 2;
 int passcred=1;if(setsockopt(pair[1],SOL_SOCKET,SO_PASSCRED,&passcred,sizeof(passcred)))return 3;
 const char write_data[]="write";char read_data[sizeof(write_data)];
 if(write(pair[0],write_data,sizeof(write_data))!=sizeof(write_data) ||
    read(pair[1],read_data,sizeof(read_data))!=sizeof(read_data) ||
    memcmp(write_data,read_data,sizeof(write_data)))return 4;
 const char to_data[]="to";char from_data[sizeof(to_data)];
 if(sendto(pair[0],to_data,sizeof(to_data),0,0,0)!=sizeof(to_data) ||
    recvfrom(pair[1],from_data,sizeof(from_data),0,0,0)!=sizeof(from_data) ||
    memcmp(to_data,from_data,sizeof(to_data)))return 5;
 int passed=open("/dev/null",O_RDONLY|O_CLOEXEC);if(passed<0)return 6;
 const char msg_data[]="msg";struct iovec send_iov={(void *)msg_data,sizeof(msg_data)};
 char send_control[CMSG_SPACE(sizeof(int))]={0};
 struct msghdr send_msg={.msg_iov=&send_iov,.msg_iovlen=1,
  .msg_control=send_control,.msg_controllen=sizeof(send_control)};
 struct cmsghdr *send_cmsg=CMSG_FIRSTHDR(&send_msg);send_cmsg->cmsg_level=SOL_SOCKET;
 send_cmsg->cmsg_type=SCM_RIGHTS;send_cmsg->cmsg_len=CMSG_LEN(sizeof(int));
 memcpy(CMSG_DATA(send_cmsg),&passed,sizeof(passed));
 if(sendmsg(pair[0],&send_msg,0)!=sizeof(msg_data))return 7;
 char recv_data[sizeof(msg_data)],recv_control[CMSG_SPACE(sizeof(struct ucred))+CMSG_SPACE(sizeof(int))]={0};
 struct iovec recv_iov={recv_data,sizeof(recv_data)};
 struct msghdr recv_msg={.msg_iov=&recv_iov,.msg_iovlen=1,
  .msg_control=recv_control,.msg_controllen=sizeof(recv_control)};
 if(recvmsg(pair[1],&recv_msg,0)!=sizeof(msg_data) || memcmp(msg_data,recv_data,sizeof(msg_data)))return 8;
 unsigned rights=0;int credentials=0;
 for(struct cmsghdr *c=CMSG_FIRSTHDR(&recv_msg);c;c=CMSG_NXTHDR(&recv_msg,c)) {
  if(c->cmsg_level!=SOL_SOCKET)continue;
  if(c->cmsg_type==SCM_RIGHTS){int received;memcpy(&received,CMSG_DATA(c),sizeof(received));close(received);rights++;}
  if(c->cmsg_type==SCM_CREDENTIALS && c->cmsg_len>=CMSG_LEN(sizeof(struct ucred)))credentials++;
 }
 close(passed);close(pair[0]);close(pair[1]);
 return rights==1 && credentials==1?0:9;
}
