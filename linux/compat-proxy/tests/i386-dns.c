/* Original i386 DNS conversion fixture: real glibc socketcall and messages. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdint.h>
int main(int argc,char **argv,char **env){
 (void)argc;(void)argv;(void)env;
 struct sockaddr_in server={.sin_family=AF_INET,.sin_port=0x3500,.sin_addr={0x632a0a7f}};
 int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return 41;
 unsigned char query[]={0x12,0x34,1,0,0,1,0,0,0,0,0,0,1,'x',0,0,1,0,1};
 struct iovec vec={query,sizeof(query)};struct msghdr m={0};
 m.msg_name=&server;m.msg_namelen=sizeof(server);m.msg_iov=&vec;m.msg_iovlen=1;
 if(sendmsg(fd,&m,0)!=sizeof(query))return 42;
 unsigned char reply[64];vec.iov_base=reply;vec.iov_len=sizeof(reply);
 struct sockaddr_in from={0};m.msg_name=&from;m.msg_namelen=sizeof(from);
 if(recvmsg(fd,&m,0)<19 || from.sin_addr.s_addr!=server.sin_addr.s_addr || from.sin_port!=server.sin_port)return 43;
 if(reply[0]!=0x12 || reply[1]!=0x34 || !(reply[2]&0x80))return 44;
 close(fd);static const char ok[]="PASS i386 DNS sendmsg/recvmsg Unix conversion and peer address\n";
 write(1,ok,sizeof(ok)-1);return 0;
}
