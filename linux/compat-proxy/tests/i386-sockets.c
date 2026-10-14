/* Original glibc/kernel ABI socket fixture. No libc startup dependency. */
#include <sys/socket.h>
#include <sys/uio.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
extern int main(int,char **,char **);
int main(int argc,char **argv,char **env){
 (void)argc;(void)argv;(void)env;
 int pair[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,pair))return 20;
 int file=open("/dev/null",O_RDONLY);if(file<0)return 21;
 char text[]="rights";struct iovec vec={text,sizeof(text)};
 union {struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int))];} control={0};
 struct msghdr message={0};message.msg_iov=&vec;message.msg_iovlen=1;
 message.msg_control=control.bytes;message.msg_controllen=sizeof(control.bytes);
 struct cmsghdr *c=CMSG_FIRSTHDR(&message);c->cmsg_len=CMSG_LEN(sizeof(int));c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;*(int *)CMSG_DATA(c)=file;
 if(sendmsg(pair[0],&message,0)!=sizeof(text))return 22;
 char reply[sizeof(text)]={0};vec.iov_base=reply;
 if(recvmsg(pair[1],&message,0)!=sizeof(text))return 23;
 c=CMSG_FIRSTHDR(&message);if(!c || c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS)return 24;
 int got=*(int *)CMSG_DATA(c);char b;if(read(got,&b,1))return 25;
 close(got);close(file);close(pair[0]);close(pair[1]);
 static const char ok[]="PASS i386 sockets, msghdr/iovec/cmsg and SCM_RIGHTS\n";
 write(1,ok,sizeof(ok)-1);return 0;
}
