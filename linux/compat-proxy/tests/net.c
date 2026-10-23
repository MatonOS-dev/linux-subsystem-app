#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sched.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
struct request {struct nlmsghdr header;struct ifinfomsg link;};
static void test(void){
    int sock=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);CHECK(sock>=0);
    struct ifreq lo={.ifr_name="lo"};CHECK(!ioctl(sock,SIOCGIFINDEX,&lo));CHECK(lo.ifr_ifindex==1);
    CHECK(!ioctl(sock,SIOCGIFFLAGS,&lo));CHECK((lo.ifr_flags&(IFF_LOOPBACK|IFF_UP))==(IFF_LOOPBACK|IFF_UP));
    short flags=lo.ifr_flags;lo.ifr_flags=(short)(flags&~IFF_UP);CHECK(!ioctl(sock,SIOCSIFFLAGS,&lo));
    CHECK(!ioctl(sock,SIOCGIFFLAGS,&lo));CHECK(!(lo.ifr_flags&IFF_UP));lo.ifr_flags=flags;CHECK(!ioctl(sock,SIOCSIFFLAGS,&lo));close(sock);
    sock=socket(AF_INET,SOCK_DGRAM,0);CHECK(sock>=0);
    lo=(struct ifreq){.ifr_ifindex=1};CHECK(!ioctl(sock,SIOCGIFNAME,&lo));CHECK(!strcmp(lo.ifr_name,"lo"));
    struct ifreq rows[2];struct ifconf conf={.ifc_len=sizeof(rows),.ifc_req=rows};
    CHECK(!ioctl(sock,SIOCGIFCONF,&conf));CHECK(conf.ifc_len==(int)sizeof(struct ifreq));
    CHECK(!strcmp(rows[0].ifr_name,"lo") && rows[0].ifr_addr.sa_family==AF_INET);close(sock);
    sock=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);CHECK(sock>=0);
    lo=(struct ifreq){.ifr_name="lo"};CHECK(!ioctl(sock,SIOCGIFINDEX,&lo));CHECK(lo.ifr_ifindex==1);close(sock);
    int fd=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,NETLINK_ROUTE);CHECK(fd>=0);
    struct sockaddr_nl local={.nl_family=AF_NETLINK,.nl_pid=(unsigned)getpid()},kernel={.nl_family=AF_NETLINK};
    CHECK(!bind(fd,(void *)&local,sizeof(local)));
    socklen_t len=sizeof(local);memset(&local,0,sizeof(local));CHECK(!getsockname(fd,(void *)&local,&len));CHECK(local.nl_pid==(unsigned)getpid());
    struct request req={.header={.nlmsg_len=sizeof(req),.nlmsg_type=RTM_NEWLINK,.nlmsg_flags=NLM_F_REQUEST|NLM_F_ACK,.nlmsg_seq=0x1234,.nlmsg_pid=local.nl_pid},.link={.ifi_index=1,.ifi_flags=IFF_UP,.ifi_change=IFF_UP}};
    struct iovec out[2]={{&req.header,sizeof(req.header)},{&req.link,sizeof(req.link)}};
    struct msghdr msg={.msg_name=&kernel,.msg_namelen=sizeof(kernel),.msg_iov=out,.msg_iovlen=2};
    CHECK(sendmsg(fd,&msg,0)==sizeof(req));
    struct pollfd p={.fd=fd,.events=POLLIN};CHECK(poll(&p,1,0)==1 && (p.revents&POLLIN));
    int alias=fcntl(fd,F_DUPFD_CLOEXEC,100);CHECK(alias>=100);close(fd);fd=alias;
    struct {struct nlmsghdr h;struct nlmsgerr error;} reply;
    struct iovec in[2]={{&reply.h,sizeof(reply.h)},{&reply.error,sizeof(reply.error)}};
    struct sockaddr_nl from;msg=(struct msghdr){.msg_name=&from,.msg_namelen=sizeof(from),.msg_iov=in,.msg_iovlen=2};
    CHECK(recvmsg(fd,&msg,MSG_PEEK)==sizeof(reply));CHECK(reply.h.nlmsg_seq==0x1234 && reply.h.nlmsg_pid==local.nl_pid && reply.h.nlmsg_type==NLMSG_ERROR);CHECK(!reply.error.error && !from.nl_pid);
    char small[3];CHECK(recv(fd,small,sizeof(small),MSG_PEEK|MSG_TRUNC)==sizeof(reply));
    CHECK(recvmsg(fd,&msg,0)==sizeof(reply));p.fd=fd;CHECK(!poll(&p,1,0));
    errno=0;CHECK(recv(fd,&reply,sizeof(reply),MSG_DONTWAIT)==-1 && errno==EAGAIN);
    req.link.ifi_index=2;req.header.nlmsg_seq++;
    CHECK(sendto(fd,&req,sizeof(req),0,(void *)&kernel,sizeof(kernel))==sizeof(req));CHECK(recv(fd,&reply,sizeof(reply),0)==sizeof(reply));CHECK(reply.error.error==-EOPNOTSUPP);
    req.header.nlmsg_len=sizeof(req)+1;errno=0;CHECK(sendto(fd,&req,sizeof(req),0,(void *)&kernel,sizeof(kernel))==-1 && errno==EINVAL);
    close(fd);
}
int main(void){
    int outside=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);CHECK(outside>=0);
    struct ifreq hostlo={.ifr_name="lo"};CHECK(!ioctl(outside,SIOCGIFINDEX,&hostlo));CHECK(hostlo.ifr_ifindex>0);close(outside);
    /* Namespace flags on clone belong only to the child, never its monitor. */
    long child=syscall(SYS_clone,CLONE_NEWNET|SIGCHLD,0,0,0,0);CHECK(child>=0);
    if(!child){test();_exit(0);}int status;CHECK(waitpid(child,&status,0)==child && status==0);
    int real=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,NETLINK_ROUTE);
    if(real>=0){char path[64],name[128];snprintf(path,sizeof(path),"/proc/self/fd/%d",real);ssize_t n=readlink(path,name,sizeof(name)-1);CHECK(n>=0);name[n]=0;CHECK(strncmp(name,"socket:",7)==0);close(real);}
    else CHECK(errno==EPERM || errno==EACCES);
    CHECK(!unshare(CLONE_NEWNET));test();
    CHECK(!unshare(CLONE_NEWUSER));int fd=open("/proc/sys/user/max_user_namespaces",O_WRONLY|O_CLOEXEC);CHECK(fd>=0);
    int alias=dup(fd);CHECK(alias>=0);close(fd);CHECK(write(alias,"1",1)==1);close(alias);
    CHECK(!unshare(CLONE_NEWUSER));errno=0;CHECK(unshare(CLONE_NEWUSER)==-1 && errno==EPERM);
    puts("PASS virtual lo ioctl/socket types, route ACKs, message iov/peek/truncation, fd aliases, clone scope and userns limit");
    return 0;
}
